#include "git-compat-util.h"
#include "http.h"
#include "pack.h"
#include "sideband.h"
#include "run-command.h"
#include "url.h"
#include "urlmatch.h"
#include "credential.h"
#include "version.h"
#include "pkt-line.h"
#include "gettext.h"
#include "transport.h"

#if LIBCURL_VERSION_NUM >= 0x070a08
long int git_curl_ipresolve = CURL_IPRESOLVE_WHATEVER;
#else
long int git_curl_ipresolve;
#endif
int active_requests;
int http_is_verbose;
size_t http_post_buffer = 16 * LARGE_PACKET_MAX;

#if LIBCURL_VERSION_NUM >= 0x070a06
#define LIBCURL_CAN_HANDLE_AUTH_ANY
#endif

static int min_curl_sessions = 1;
static int curl_session_count;
#ifdef USE_CURL_MULTI
static int max_requests = -1;
static CURLM *curlm;
#endif
#ifndef NO_CURL_EASY_DUPHANDLE
static CURL *curl_default;
#endif

#define PREV_BUF_SIZE 4096

char curl_errorstr[CURL_ERROR_SIZE];

static int curl_ssl_verify = -1;
static int curl_ssl_try;
static const char *ssl_cert;
static const char *ssl_cipherlist;
static const char *ssl_version;
static struct {
	const char *name;
	long ssl_version;
} sslversions[] = {
	{ "sslv2", CURL_SSLVERSION_SSLv2 },
	{ "sslv3", CURL_SSLVERSION_SSLv3 },
	{ "tlsv1", CURL_SSLVERSION_TLSv1 },
#if LIBCURL_VERSION_NUM >= 0x072200
	{ "tlsv1.0", CURL_SSLVERSION_TLSv1_0 },
	{ "tlsv1.1", CURL_SSLVERSION_TLSv1_1 },
	{ "tlsv1.2", CURL_SSLVERSION_TLSv1_2 },
#endif
};
#if LIBCURL_VERSION_NUM >= 0x070903
static const char *ssl_key;
#endif
#if LIBCURL_VERSION_NUM >= 0x070908
static const char *ssl_capath;
#endif
#if LIBCURL_VERSION_NUM >= 0x072c00
static const char *ssl_pinnedkey;
#endif
static const char *ssl_cainfo;
static long curl_low_speed_limit = -1;
static long curl_low_speed_time = -1;
static int curl_ftp_no_epsv;
static const char *curl_http_proxy;
static const char *curl_no_proxy;
static const char *http_proxy_authmethod;
static struct {
	const char *name;
	long curlauth_param;
} proxy_authmethods[] = {
	{ "basic", CURLAUTH_BASIC },
	{ "digest", CURLAUTH_DIGEST },
	{ "negotiate", CURLAUTH_GSSNEGOTIATE },
	{ "ntlm", CURLAUTH_NTLM },
#ifdef LIBCURL_CAN_HANDLE_AUTH_ANY
	{ "anyauth", CURLAUTH_ANY },
#endif
	/*
	 * CURLAUTH_DIGEST_IE has no corresponding command-line option in
	 * curl(1) and is not included in CURLAUTH_ANY, so we leave it out
	 * here, too
	 */
};
static struct credential proxy_auth = CREDENTIAL_INIT;
static const char *curl_proxyuserpwd;
static const char *curl_cookie_file;
static int curl_save_cookies;
struct credential http_auth = CREDENTIAL_INIT;
static int http_proactive_auth;
static const char *user_agent;
static int curl_empty_auth;

#if LIBCURL_VERSION_NUM >= 0x071700
/* Use CURLOPT_KEYPASSWD as is */
#elif LIBCURL_VERSION_NUM >= 0x070903
#define CURLOPT_KEYPASSWD CURLOPT_SSLKEYPASSWD
#else
#define CURLOPT_KEYPASSWD CURLOPT_SSLCERTPASSWD
#endif

static struct credential cert_auth = CREDENTIAL_INIT;
static int ssl_cert_password_required;
#ifdef LIBCURL_CAN_HANDLE_AUTH_ANY
static unsigned long http_auth_methods = CURLAUTH_ANY;
#endif

static struct curl_slist *pragma_header;
static struct curl_slist *no_pragma_header;
static struct curl_slist *extra_http_headers;

static struct active_request_slot *active_queue_head;

static char *cached_accept_language;

size_t fread_buffer(char *ptr, size_t eltsize, size_t nmemb, void *buffer_)
{
	size_t size = eltsize * nmemb;
	struct buffer *buffer = buffer_;

	if (size > buffer->buf.len - buffer->posn)
		size = buffer->buf.len - buffer->posn;
	memcpy(ptr, buffer->buf.buf + buffer->posn, size);
	buffer->posn += size;

	return size;
}

#ifndef NO_CURL_IOCTL
curlioerr ioctl_buffer(CURL *handle, int cmd, void *clientp)
{
	struct buffer *buffer = clientp;

	switch (cmd) {
	case CURLIOCMD_NOP:
		return CURLIOE_OK;

	case CURLIOCMD_RESTARTREAD:
		buffer->posn = 0;
		return CURLIOE_OK;

	default:
		return CURLIOE_UNKNOWNCMD;
	}
}
#endif

size_t fwrite_buffer(char *ptr, size_t eltsize, size_t nmemb, void *buffer_)
{
	size_t size = eltsize * nmemb;
	struct strbuf *buffer = buffer_;

	strbuf_add(buffer, ptr, size);
	return size;
}

size_t fwrite_null(char *ptr, size_t eltsize, size_t nmemb, void *strbuf)
{
	return eltsize * nmemb;
}

static void closedown_active_slot(struct active_request_slot *slot)
{
	active_requests--;
	slot->in_use = 0;
}

static void finish_active_slot(struct active_request_slot *slot)
{
	closedown_active_slot(slot);
	curl_easy_getinfo(slot->curl, CURLINFO_HTTP_CODE, &slot->http_code);

	if (slot->finished != NULL)
		(*slot->finished) = 1;

	/* Store slot results so they can be read after the slot is reused */
	if (slot->results != NULL) {
		slot->results->curl_result = slot->curl_result;
		slot->results->http_code = slot->http_code;
#if LIBCURL_VERSION_NUM >= 0x070a08
		curl_easy_getinfo(slot->curl, CURLINFO_HTTPAUTH_AVAIL,
				  &slot->results->auth_avail);
#else
		slot->results->auth_avail = 0;
#endif

		curl_easy_getinfo(slot->curl, CURLINFO_HTTP_CONNECTCODE,
			&slot->results->http_connectcode);
	}

	/* Run callback if appropriate */
	if (slot->callback_func != NULL)
		slot->callback_func(slot->callback_data);
}

#ifdef USE_CURL_MULTI
static void process_curl_messages(void)
{
	int num_messages;
	struct active_request_slot *slot;
	CURLMsg *curl_message = curl_multi_info_read(curlm, &num_messages);

	while (curl_message != NULL) {
		if (curl_message->msg == CURLMSG_DONE) {
			int curl_result = curl_message->data.result;
			slot = active_queue_head;
			while (slot != NULL &&
			       slot->curl != curl_message->easy_handle)
				slot = slot->next;
			if (slot != NULL) {
				curl_multi_remove_handle(curlm, slot->curl);
				slot->curl_result = curl_result;
				finish_active_slot(slot);
			} else {
				fprintf(stderr, "Received DONE message for unknown request!\n");
			}
		} else {
			fprintf(stderr, "Unknown CURL message received: %d\n",
				(int)curl_message->msg);
		}
		curl_message = curl_multi_info_read(curlm, &num_messages);
	}
}
#endif

static int http_options(const char *var, const char *value, void *cb)
{
	if (!strcmp("http.sslverify", var)) {
		curl_ssl_verify = git_config_bool(var, value);
		return 0;
	}
	if (!strcmp("http.sslcipherlist", var))
		return git_config_string(&ssl_cipherlist, var, value);
	if (!strcmp("http.sslversion", var))
		return git_config_string(&ssl_version, var, value);
	if (!strcmp("http.sslcert", var))
		return git_config_pathname(&ssl_cert, var, value);
#if LIBCURL_VERSION_NUM >= 0x070903
	if (!strcmp("http.sslkey", var))
		return git_config_pathname(&ssl_key, var, value);
#endif
#if LIBCURL_VERSION_NUM >= 0x070908
	if (!strcmp("http.sslcapath", var))
		return git_config_pathname(&ssl_capath, var, value);
#endif
	if (!strcmp("http.sslcainfo", var))
		return git_config_pathname(&ssl_cainfo, var, value);
	if (!strcmp("http.sslcertpasswordprotected", var)) {
		ssl_cert_password_required = git_config_bool(var, value);
		return 0;
	}
	if (!strcmp("http.ssltry", var)) {
		curl_ssl_try = git_config_bool(var, value);
		return 0;
	}
	if (!strcmp("http.minsessions", var)) {
		min_curl_sessions = git_config_int(var, value);
#ifndef USE_CURL_MULTI
		if (min_curl_sessions > 1)
			min_curl_sessions = 1;
#endif
		return 0;
	}
#ifdef USE_CURL_MULTI
	if (!strcmp("http.maxrequests", var)) {
		max_requests = git_config_int(var, value);
		return 0;
	}
#endif
	if (!strcmp("http.lowspeedlimit", var)) {
		curl_low_speed_limit = (long)git_config_int(var, value);
		return 0;
	}
	if (!strcmp("http.lowspeedtime", var)) {
		curl_low_speed_time = (long)git_config_int(var, value);
		return 0;
	}

	if (!strcmp("http.noepsv", var)) {
		curl_ftp_no_epsv = git_config_bool(var, value);
		return 0;
	}
	if (!strcmp("http.proxy", var))
		return git_config_string(&curl_http_proxy, var, value);

	if (!strcmp("http.proxyauthmethod", var))
		return git_config_string(&http_proxy_authmethod, var, value);

	if (!strcmp("http.cookiefile", var))
		return git_config_pathname(&curl_cookie_file, var, value);
	if (!strcmp("http.savecookies", var)) {
		curl_save_cookies = git_config_bool(var, value);
		return 0;
	}

	if (!strcmp("http.postbuffer", var)) {
		http_post_buffer = git_config_int(var, value);
		if (http_post_buffer < LARGE_PACKET_MAX)
			http_post_buffer = LARGE_PACKET_MAX;
		return 0;
	}

	if (!strcmp("http.useragent", var))
		return git_config_string(&user_agent, var, value);

	if (!strcmp("http.emptyauth", var)) {
		curl_empty_auth = git_config_bool(var, value);
		return 0;
	}

	if (!strcmp("http.pinnedpubkey", var)) {
#if LIBCURL_VERSION_NUM >= 0x072c00
		return git_config_pathname(&ssl_pinnedkey, var, value);
#else
		warning(_("Public key pinning not supported with cURL < 7.44.0"));
		return 0;
#endif
	}

	if (!strcmp("http.extraheader", var)) {
		if (!value) {
			return config_error_nonbool(var);
		} else if (!*value) {
			curl_slist_free_all(extra_http_headers);
			extra_http_headers = NULL;
		} else {
			extra_http_headers =
				curl_slist_append(extra_http_headers, value);
		}
		return 0;
	}

	/* Fall back on the default ones */
	return git_default_config(var, value, cb);
}

static void init_curl_http_auth(CURL *result)
{
	if (!http_auth.username) {
		if (curl_empty_auth)
			curl_easy_setopt(result, CURLOPT_USERPWD, ":");
		return;
	}

	credential_fill(&http_auth);

#if LIBCURL_VERSION_NUM >= 0x071301
	curl_easy_setopt(result, CURLOPT_USERNAME, http_auth.username);
	curl_easy_setopt(result, CURLOPT_PASSWORD, http_auth.password);
#else
	{
		static struct strbuf up = STRBUF_INIT;
		/*
		 * Note that we assume we only ever have a single set of
		 * credentials in a given program run, so we do not have
		 * to worry about updating this buffer, only setting its
		 * initial value.
		 */
		if (!up.len)
			strbuf_addf(&up, "%s:%s",
				http_auth.username, http_auth.password);
		curl_easy_setopt(result, CURLOPT_USERPWD, up.buf);
	}
#endif
}

/* *var must be free-able */
static void var_override(const char **var, char *value)
{
	if (value) {
		free((void *)*var);
		*var = xstrdup(value);
	}
}

static void set_proxyauth_name_password(CURL *result)
{
#if LIBCURL_VERSION_NUM >= 0x071301
		curl_easy_setopt(result, CURLOPT_PROXYUSERNAME,
			proxy_auth.username);
		curl_easy_setopt(result, CURLOPT_PROXYPASSWORD,
			proxy_auth.password);
#else
		struct strbuf s = STRBUF_INIT;

		strbuf_addstr_urlencode(&s, proxy_auth.username, 1);
		strbuf_addch(&s, ':');
		strbuf_addstr_urlencode(&s, proxy_auth.password, 1);
		curl_proxyuserpwd = strbuf_detach(&s, NULL);
		curl_easy_setopt(result, CURLOPT_PROXYUSERPWD, curl_proxyuserpwd);
#endif
}

static void init_curl_proxy_auth(CURL *result)
{
	if (proxy_auth.username) {
		if (!proxy_auth.password)
			credential_fill(&proxy_auth);
		set_proxyauth_name_password(result);
	}

	var_override(&http_proxy_authmethod, getenv("GIT_HTTP_PROXY_AUTHMETHOD"));

#if LIBCURL_VERSION_NUM >= 0x070a07 /* CURLOPT_PROXYAUTH and CURLAUTH_ANY */
	if (http_proxy_authmethod) {
		int i;
		for (i = 0; i < ARRAY_SIZE(proxy_authmethods); i++) {
			if (!strcmp(http_proxy_authmethod, proxy_authmethods[i].name)) {
				curl_easy_setopt(result, CURLOPT_PROXYAUTH,
						proxy_authmethods[i].curlauth_param);
				break;
			}
		}
		if (i == ARRAY_SIZE(proxy_authmethods)) {
			warning("unsupported proxy authentication method %s: using anyauth",
					http_proxy_authmethod);
			curl_easy_setopt(result, CURLOPT_PROXYAUTH, CURLAUTH_ANY);
		}
	}
	else
		curl_easy_setopt(result, CURLOPT_PROXYAUTH, CURLAUTH_ANY);
#endif
}

static int has_cert_password(void)
{
	if (ssl_cert == NULL || ssl_cert_password_required != 1)
		return 0;
	if (!cert_auth.password) {
		cert_auth.protocol = xstrdup("cert");
		cert_auth.username = xstrdup("");
		cert_auth.path = xstrdup(ssl_cert);
		credential_fill(&cert_auth);
	}
	return 1;
}

#if LIBCURL_VERSION_NUM >= 0x071900
static void set_curl_keepalive(CURL *c)
{
	curl_easy_setopt(c, CURLOPT_TCP_KEEPALIVE, 1);
}

#elif LIBCURL_VERSION_NUM >= 0x071000
static int sockopt_callback(void *client, curl_socket_t fd, curlsocktype type)
{
	int ka = 1;
	int rc;
	socklen_t len = (socklen_t)sizeof(ka);

	if (type != CURLSOCKTYPE_IPCXN)
		return 0;

	rc = setsockopt(fd, SOL_SOCKET, SO_KEEPALIVE, (void *)&ka, len);
	if (rc < 0)
		warning_errno("unable to set SO_KEEPALIVE on socket");

	return 0; /* CURL_SOCKOPT_OK only exists since curl 7.21.5 */
}

static void set_curl_keepalive(CURL *c)
{
	curl_easy_setopt(c, CURLOPT_SOCKOPTFUNCTION, sockopt_callback);
}

#else
static void set_curl_keepalive(CURL *c)
{
	/* not supported on older curl versions */
}
#endif

static CURL *get_curl_handle(void)
{
	CURL *result = curl_easy_init();
	long allowed_protocols = 0;

	if (!result)
		die("curl_easy_init failed");

	if (!curl_ssl_verify) {
		curl_easy_setopt(result, CURLOPT_SSL_VERIFYPEER, 0);
		curl_easy_setopt(result, CURLOPT_SSL_VERIFYHOST, 0);
	} else {
		/* Verify authenticity of the peer's certificate */
		curl_easy_setopt(result, CURLOPT_SSL_VERIFYPEER, 1);
		/* The name in the cert must match whom we tried to connect */
		curl_easy_setopt(result, CURLOPT_SSL_VERIFYHOST, 2);
	}

#if LIBCURL_VERSION_NUM >= 0x070907
	curl_easy_setopt(result, CURLOPT_NETRC, CURL_NETRC_OPTIONAL);
#endif
#ifdef LIBCURL_CAN_HANDLE_AUTH_ANY
	curl_easy_setopt(result, CURLOPT_HTTPAUTH, CURLAUTH_ANY);
#endif

	if (http_proactive_auth)
		init_curl_http_auth(result);

	if (getenv("GIT_SSL_VERSION"))
		ssl_version = getenv("GIT_SSL_VERSION");
	if (ssl_version && *ssl_version) {
		int i;
		for (i = 0; i < ARRAY_SIZE(sslversions); i++) {
			if (!strcmp(ssl_version, sslversions[i].name)) {
				curl_easy_setopt(result, CURLOPT_SSLVERSION,
						 sslversions[i].ssl_version);
				break;
			}
		}
		if (i == ARRAY_SIZE(sslversions))
			warning("unsupported ssl version %s: using default",
				ssl_version);
	}

	if (getenv("GIT_SSL_CIPHER_LIST"))
		ssl_cipherlist = getenv("GIT_SSL_CIPHER_LIST");
	if (ssl_cipherlist != NULL && *ssl_cipherlist)
		curl_easy_setopt(result, CURLOPT_SSL_CIPHER_LIST,
				ssl_cipherlist);

	if (ssl_cert != NULL)
		curl_easy_setopt(result, CURLOPT_SSLCERT, ssl_cert);
	if (has_cert_password())
		curl_easy_setopt(result, CURLOPT_KEYPASSWD, cert_auth.password);
#if LIBCURL_VERSION_NUM >= 0x070903
	if (ssl_key != NULL)
		curl_easy_setopt(result, CURLOPT_SSLKEY, ssl_key);
#endif
#if LIBCURL_VERSION_NUM >= 0x070908
	if (ssl_capath != NULL)
		curl_easy_setopt(result, CURLOPT_CAPATH, ssl_capath);
#endif
#if LIBCURL_VERSION_NUM >= 0x072c00
	if (ssl_pinnedkey != NULL)
		curl_easy_setopt(result, CURLOPT_PINNEDPUBLICKEY, ssl_pinnedkey);
#endif
	if (ssl_cainfo != NULL)
		curl_easy_setopt(result, CURLOPT_CAINFO, ssl_cainfo);

	if (curl_low_speed_limit > 0 && curl_low_speed_time > 0) {
		curl_easy_setopt(result, CURLOPT_LOW_SPEED_LIMIT,
				 curl_low_speed_limit);
		curl_easy_setopt(result, CURLOPT_LOW_SPEED_TIME,
				 curl_low_speed_time);
	}

	curl_easy_setopt(result, CURLOPT_FOLLOWLOCATION, 1);
	curl_easy_setopt(result, CURLOPT_MAXREDIRS, 20);
#if LIBCURL_VERSION_NUM >= 0x071301
	curl_easy_setopt(result, CURLOPT_POSTREDIR, CURL_REDIR_POST_ALL);
#elif LIBCURL_VERSION_NUM >= 0x071101
	curl_easy_setopt(result, CURLOPT_POST301, 1);
#endif
#if LIBCURL_VERSION_NUM >= 0x071304
	if (is_transport_allowed("http"))
		allowed_protocols |= CURLPROTO_HTTP;
	if (is_transport_allowed("https"))
		allowed_protocols |= CURLPROTO_HTTPS;
	if (is_transport_allowed("ftp"))
		allowed_protocols |= CURLPROTO_FTP;
	if (is_transport_allowed("ftps"))
		allowed_protocols |= CURLPROTO_FTPS;
	curl_easy_setopt(result, CURLOPT_REDIR_PROTOCOLS, allowed_protocols);
#else
	if (transport_restrict_protocols())
		warning("protocol restrictions not applied to curl redirects because\n"
			"your curl version is too old (>= 7.19.4)");
#endif

	if (getenv("GIT_CURL_VERBOSE"))
		curl_easy_setopt(result, CURLOPT_VERBOSE, 1);

	curl_easy_setopt(result, CURLOPT_USERAGENT,
		user_agent ? user_agent : git_user_agent());

	if (curl_ftp_no_epsv)
		curl_easy_setopt(result, CURLOPT_FTP_USE_EPSV, 0);

#ifdef CURLOPT_USE_SSL
	if (curl_ssl_try)
		curl_easy_setopt(result, CURLOPT_USE_SSL, CURLUSESSL_TRY);
#endif

	/*
	 * CURL also examines these variables as a fallback; but we need to query
	 * them here in order to decide whether to prompt for missing password (cf.
	 * init_curl_proxy_auth()).
	 *
	 * Unlike many other common environment variables, these are historically
	 * lowercase only. It appears that CURL did not know this and implemented
	 * only uppercase variants, which was later corrected to take both - with
	 * the exception of http_proxy, which is lowercase only also in CURL. As
	 * the lowercase versions are the historical quasi-standard, they take
	 * precedence here, as in CURL.
	 */
	if (!curl_http_proxy) {
		if (!strcmp(http_auth.protocol, "https")) {
			var_override(&curl_http_proxy, getenv("HTTPS_PROXY"));
			var_override(&curl_http_proxy, getenv("https_proxy"));
		} else {
			var_override(&curl_http_proxy, getenv("http_proxy"));
		}
		if (!curl_http_proxy) {
			var_override(&curl_http_proxy, getenv("ALL_PROXY"));
			var_override(&curl_http_proxy, getenv("all_proxy"));
		}
	}

	if (curl_http_proxy) {
		curl_easy_setopt(result, CURLOPT_PROXY, curl_http_proxy);
#if LIBCURL_VERSION_NUM >= 0x071800
		if (starts_with(curl_http_proxy, "socks5h"))
			curl_easy_setopt(result,
				CURLOPT_PROXYTYPE, CURLPROXY_SOCKS5_HOSTNAME);
		else if (starts_with(curl_http_proxy, "socks5"))
			curl_easy_setopt(result,
				CURLOPT_PROXYTYPE, CURLPROXY_SOCKS5);
		else if (starts_with(curl_http_proxy, "socks4a"))
			curl_easy_setopt(result,
				CURLOPT_PROXYTYPE, CURLPROXY_SOCKS4A);
		else if (starts_with(curl_http_proxy, "socks"))
			curl_easy_setopt(result,
				CURLOPT_PROXYTYPE, CURLPROXY_SOCKS4);
#endif
		if (strstr(curl_http_proxy, "://"))
			credential_from_url(&proxy_auth, curl_http_proxy);
		else {
			struct strbuf url = STRBUF_INIT;
			strbuf_addf(&url, "http://%s", curl_http_proxy);
			credential_from_url(&proxy_auth, url.buf);
			strbuf_release(&url);
		}

		curl_easy_setopt(result, CURLOPT_PROXY, proxy_auth.host);
#if LIBCURL_VERSION_NUM >= 0x071304
		var_override(&curl_no_proxy, getenv("NO_PROXY"));
		var_override(&curl_no_proxy, getenv("no_proxy"));
		curl_easy_setopt(result, CURLOPT_NOPROXY, curl_no_proxy);
#endif
	}
	init_curl_proxy_auth(result);

	set_curl_keepalive(result);

	return result;
}

static void set_from_env(const char **var, const char *envname)
{
	const char *val = getenv(envname);
	if (val)
		*var = val;
}

void http_init(struct remote *remote, const char *url, int proactive_auth)
{
	char *low_speed_limit;
	char *low_speed_time;
	char *normalized_url;
	struct urlmatch_config config = { STRING_LIST_INIT_DUP };

	config.section = "http";
	config.key = NULL;
	config.collect_fn = http_options;
	config.cascade_fn = git_default_config;
	config.cb = NULL;

	http_is_verbose = 0;
	normalized_url = url_normalize(url, &config.url);

	git_config(urlmatch_config_entry, &config);
	free(normalized_url);

	if (curl_global_init(CURL_GLOBAL_ALL) != CURLE_OK)
		die("curl_global_init failed");

	http_proactive_auth = proactive_auth;

	if (remote && remote->http_proxy)
		curl_http_proxy = xstrdup(remote->http_proxy);

	if (remote)
		var_override(&http_proxy_authmethod, remote->http_proxy_authmethod);

	pragma_header = curl_slist_append(http_copy_default_headers(),
		"Pragma: no-cache");
	no_pragma_header = curl_slist_append(http_copy_default_headers(),
		"Pragma:");

#ifdef USE_CURL_MULTI
	{
		char *http_max_requests = getenv("GIT_HTTP_MAX_REQUESTS");
		if (http_max_requests != NULL)
			max_requests = atoi(http_max_requests);
	}

	curlm = curl_multi_init();
	if (!curlm)
		die("curl_multi_init failed");
#endif

	if (getenv("GIT_SSL_NO_VERIFY"))
		curl_ssl_verify = 0;

	set_from_env(&ssl_cert, "GIT_SSL_CERT");
#if LIBCURL_VERSION_NUM >= 0x070903
	set_from_env(&ssl_key, "GIT_SSL_KEY");
#endif
#if LIBCURL_VERSION_NUM >= 0x070908
	set_from_env(&ssl_capath, "GIT_SSL_CAPATH");
#endif
	set_from_env(&ssl_cainfo, "GIT_SSL_CAINFO");

	set_from_env(&user_agent, "GIT_HTTP_USER_AGENT");

	low_speed_limit = getenv("GIT_HTTP_LOW_SPEED_LIMIT");
	if (low_speed_limit != NULL)
		curl_low_speed_limit = strtol(low_speed_limit, NULL, 10);
	low_speed_time = getenv("GIT_HTTP_LOW_SPEED_TIME");
	if (low_speed_time != NULL)
		curl_low_speed_time = strtol(low_speed_time, NULL, 10);

	if (curl_ssl_verify == -1)
		curl_ssl_verify = 1;

	curl_session_count = 0;
#ifdef USE_CURL_MULTI
	if (max_requests < 1)
		max_requests = DEFAULT_MAX_REQUESTS;
#endif

	if (getenv("GIT_CURL_FTP_NO_EPSV"))
		curl_ftp_no_epsv = 1;

	if (url) {
		credential_from_url(&http_auth, url);
		if (!ssl_cert_password_required &&
		    getenv("GIT_SSL_CERT_PASSWORD_PROTECTED") &&
		    starts_with(url, "https://"))
			ssl_cert_password_required = 1;
	}

#ifndef NO_CURL_EASY_DUPHANDLE
	curl_default = get_curl_handle();
#endif
}

void http_cleanup(void)
{
	struct active_request_slot *slot = active_queue_head;

	while (slot != NULL) {
		struct active_request_slot *next = slot->next;
		if (slot->curl != NULL) {
#ifdef USE_CURL_MULTI
			curl_multi_remove_handle(curlm, slot->curl);
#endif
			curl_easy_cleanup(slot->curl);
		}
		free(slot);
		slot = next;
	}
	active_queue_head = NULL;

#ifndef NO_CURL_EASY_DUPHANDLE
	curl_easy_cleanup(curl_default);
#endif

#ifdef USE_CURL_MULTI
	curl_multi_cleanup(curlm);
#endif
	curl_global_cleanup();

	curl_slist_free_all(extra_http_headers);
	extra_http_headers = NULL;

	curl_slist_free_all(pragma_header);
	pragma_header = NULL;

	curl_slist_free_all(no_pragma_header);
	no_pragma_header = NULL;

	if (curl_http_proxy) {
		free((void *)curl_http_proxy);
		curl_http_proxy = NULL;
	}

	if (proxy_auth.password) {
		memset(proxy_auth.password, 0, strlen(proxy_auth.password));
		free(proxy_auth.password);
		proxy_auth.password = NULL;
	}

	free((void *)curl_proxyuserpwd);
	curl_proxyuserpwd = NULL;

	free((void *)http_proxy_authmethod);
	http_proxy_authmethod = NULL;

	if (cert_auth.password != NULL) {
		memset(cert_auth.password, 0, strlen(cert_auth.password));
		free(cert_auth.password);
		cert_auth.password = NULL;
	}
	ssl_cert_password_required = 0;

	free(cached_accept_language);
	cached_accept_language = NULL;
}

struct active_request_slot *get_active_slot(void)
{
	struct active_request_slot *slot = active_queue_head;
	struct active_request_slot *newslot;

#ifdef USE_CURL_MULTI
	int num_transfers;

	/* Wait for a slot to open up if the queue is full */
	while (active_requests >= max_requests) {
		curl_multi_perform(curlm, &num_transfers);
		if (num_transfers < active_requests)
			process_curl_messages();
	}
#endif

	while (slot != NULL && slot->in_use)
		slot = slot->next;

	if (slot == NULL) {
		newslot = xmalloc(sizeof(*newslot));
		newslot->curl = NULL;
		newslot->in_use = 0;
		newslot->next = NULL;

		slot = active_queue_head;
		if (slot == NULL) {
			active_queue_head = newslot;
		} else {
			while (slot->next != NULL)
				slot = slot->next;
			slot->next = newslot;
		}
		slot = newslot;
	}

	if (slot->curl == NULL) {
#ifdef NO_CURL_EASY_DUPHANDLE
		slot->curl = get_curl_handle();
#else
		slot->curl = curl_easy_duphandle(curl_default);
#endif
		curl_session_count++;
	}

	active_requests++;
	slot->in_use = 1;
	slot->results = NULL;
	slot->finished = NULL;
	slot->callback_data = NULL;
	slot->callback_func = NULL;
	curl_easy_setopt(slot->curl, CURLOPT_COOKIEFILE, curl_cookie_file);
	if (curl_save_cookies)
		curl_easy_setopt(slot->curl, CURLOPT_COOKIEJAR, curl_cookie_file);
	curl_easy_setopt(slot->curl, CURLOPT_HTTPHEADER, pragma_header);
	curl_easy_setopt(slot->curl, CURLOPT_ERRORBUFFER, curl_errorstr);
	curl_easy_setopt(slot->curl, CURLOPT_CUSTOMREQUEST, NULL);
	curl_easy_setopt(slot->curl, CURLOPT_READFUNCTION, NULL);
	curl_easy_setopt(slot->curl, CURLOPT_WRITEFUNCTION, NULL);
	curl_easy_setopt(slot->curl, CURLOPT_POSTFIELDS, NULL);
	curl_easy_setopt(slot->curl, CURLOPT_UPLOAD, 0);
	curl_easy_setopt(slot->curl, CURLOPT_HTTPGET, 1);
	curl_easy_setopt(slot->curl, CURLOPT_FAILONERROR, 1);
	curl_easy_setopt(slot->curl, CURLOPT_RANGE, NULL);

#if LIBCURL_VERSION_NUM >= 0x070a08
	curl_easy_setopt(slot->curl, CURLOPT_IPRESOLVE, git_curl_ipresolve);
#endif
#ifdef LIBCURL_CAN_HANDLE_AUTH_ANY
	curl_easy_setopt(slot->curl, CURLOPT_HTTPAUTH, http_auth_methods);
#endif
	if (http_auth.password || curl_empty_auth)
		init_curl_http_auth(slot->curl);

	return slot;
}

int start_active_slot(struct active_request_slot *slot)
{
#ifdef USE_CURL_MULTI
	CURLMcode curlm_result = curl_multi_add_handle(curlm, slot->curl);
	int num_transfers;

	if (curlm_result != CURLM_OK &&
	    curlm_result != CURLM_CALL_MULTI_PERFORM) {
		active_requests--;
		slot->in_use = 0;
		return 0;
	}

	/*
	 * We know there must be something to do, since we just added
	 * something.
	 */
	curl_multi_perform(curlm, &num_transfers);
#endif
	return 1;
}

#ifdef USE_CURL_MULTI
struct fill_chain {
	void *data;
	int (*fill)(void *);
	struct fill_chain *next;
};

static struct fill_chain *fill_cfg;

void add_fill_function(void *data, int (*fill)(void *))
{
	struct fill_chain *new = xmalloc(sizeof(*new));
	struct fill_chain **linkp = &fill_cfg;
	new->data = data;
	new->fill = fill;
	new->next = NULL;
	while (*linkp)
		linkp = &(*linkp)->next;
	*linkp = new;
}

void fill_active_slots(void)
{
	struct active_request_slot *slot = active_queue_head;

	while (active_requests < max_requests) {
		struct fill_chain *fill;
		for (fill = fill_cfg; fill; fill = fill->next)
			if (fill->fill(fill->data))
				break;

		if (!fill)
			break;
	}

	while (slot != NULL) {
		if (!slot->in_use && slot->curl != NULL
			&& curl_session_count > min_curl_sessions) {
			curl_easy_cleanup(slot->curl);
			slot->curl = NULL;
			curl_session_count--;
		}
		slot = slot->next;
	}
}

void step_active_slots(void)
{
	int num_transfers;
	CURLMcode curlm_result;

	do {
		curlm_result = curl_multi_perform(curlm, &num_transfers);
	} while (curlm_result == CURLM_CALL_MULTI_PERFORM);
	if (num_transfers < active_requests) {
		process_curl_messages();
		fill_active_slots();
	}
}
#endif

void run_active_slot(struct active_request_slot *slot)
{
#ifdef USE_CURL_MULTI
	fd_set readfds;
	fd_set writefds;
	fd_set excfds;
	int max_fd;
	struct timeval select_timeout;
	int finished = 0;

	slot->finished = &finished;
	while (!finished) {
		step_active_slots();

		if (slot->in_use) {
#if LIBCURL_VERSION_NUM >= 0x070f04
			long curl_timeout;
			curl_multi_timeout(curlm, &curl_timeout);
			if (curl_timeout == 0) {
				continue;
			} else if (curl_timeout == -1) {
				select_timeout.tv_sec  = 0;
				select_timeout.tv_usec = 50000;
			} else {
				select_timeout.tv_sec  =  curl_timeout / 1000;
				select_timeout.tv_usec = (curl_timeout % 1000) * 1000;
			}
#else
			select_timeout.tv_sec  = 0;
			select_timeout.tv_usec = 50000;
#endif

			max_fd = -1;
			FD_ZERO(&readfds);
			FD_ZERO(&writefds);
			FD_ZERO(&excfds);
			curl_multi_fdset(curlm, &readfds, &writefds, &excfds, &max_fd);

			/*
			 * It can happen that curl_multi_timeout returns a pathologically
			 * long timeout when curl_multi_fdset returns no file descriptors
			 * to read.  See commit message for more details.
			 */
			if (max_fd < 0 &&
			    (select_timeout.tv_sec > 0 ||
			     select_timeout.tv_usec > 50000)) {
				select_timeout.tv_sec  = 0;
				select_timeout.tv_usec = 50000;
			}

			select(max_fd+1, &readfds, &writefds, &excfds, &select_timeout);
		}
	}
#else
	while (slot->in_use) {
		slot->curl_result = curl_easy_perform(slot->curl);
		finish_active_slot(slot);
	}
#endif
}

static void release_active_slot(struct active_request_slot *slot)
{
	closedown_active_slot(slot);
	if (slot->curl && curl_session_count > min_curl_sessions) {
#ifdef USE_CURL_MULTI
		curl_multi_remove_handle(curlm, slot->curl);
#endif
		curl_easy_cleanup(slot->curl);
		slot->curl = NULL;
		curl_session_count--;
	}
#ifdef USE_CURL_MULTI
	fill_active_slots();
#endif
}

void finish_all_active_slots(void)
{
	struct active_request_slot *slot = active_queue_head;

	while (slot != NULL)
		if (slot->in_use) {
			run_active_slot(slot);
			slot = active_queue_head;
		} else {
			slot = slot->next;
		}
}

/* Helpers for modifying and creating URLs */
static inline int needs_quote(int ch)
{
	if (((ch >= 'A') && (ch <= 'Z'))
			|| ((ch >= 'a') && (ch <= 'z'))
			|| ((ch >= '0') && (ch <= '9'))
			|| (ch == '/')
			|| (ch == '-')
			|| (ch == '.'))
		return 0;
	return 1;
}

static char *quote_ref_url(const char *base, const char *ref)
{
	struct strbuf buf = STRBUF_INIT;
	const char *cp;
	int ch;

	end_url_with_slash(&buf, base);

	for (cp = ref; (ch = *cp) != 0; cp++)
		if (needs_quote(ch))
			strbuf_addf(&buf, "%%%02x", ch);
		else
			strbuf_addch(&buf, *cp);

	return strbuf_detach(&buf, NULL);
}

void append_remote_object_url(struct strbuf *buf, const char *url,
			      const char *hex,
			      int only_two_digit_prefix)
{
	end_url_with_slash(buf, url);

	strbuf_addf(buf, "objects/%.*s/", 2, hex);
	if (!only_two_digit_prefix)
		strbuf_addstr(buf, hex + 2);
}

char *get_remote_object_url(const char *url, const char *hex,
			    int only_two_digit_prefix)
{
	struct strbuf buf = STRBUF_INIT;
	append_remote_object_url(&buf, url, hex, only_two_digit_prefix);
	return strbuf_detach(&buf, NULL);
}

static int handle_curl_result(struct slot_results *results)
{
	/*
	 * If we see a failing http code with CURLE_OK, we have turned off
	 * FAILONERROR (to keep the server's custom error response), and should
	 * translate the code into failure here.
	 */
	if (results->curl_result == CURLE_OK &&
	    results->http_code >= 400) {
		results->curl_result = CURLE_HTTP_RETURNED_ERROR;
		/*
		 * Normally curl will already have put the "reason phrase"
		 * from the server into curl_errorstr; unfortunately without
		 * FAILONERROR it is lost, so we can give only the numeric
		 * status code.
		 */
		snprintf(curl_errorstr, sizeof(curl_errorstr),
			 "The requested URL returned error: %ld",
			 results->http_code);
	}

	if (results->curl_result == CURLE_OK) {
		credential_approve(&http_auth);
		if (proxy_auth.password)
			credential_approve(&proxy_auth);
		return HTTP_OK;
	} else if (missing_target(results))
		return HTTP_MISSING_TARGET;
	else if (results->http_code == 401) {
		if (http_auth.username && http_auth.password) {
			credential_reject(&http_auth);
			return HTTP_NOAUTH;
		} else {
#ifdef LIBCURL_CAN_HANDLE_AUTH_ANY
			http_auth_methods &= ~CURLAUTH_GSSNEGOTIATE;
#endif
			return HTTP_REAUTH;
		}
	} else {
		if (results->http_connectcode == 407)
			credential_reject(&proxy_auth);
#if LIBCURL_VERSION_NUM >= 0x070c00
		if (!curl_errorstr[0])
			strlcpy(curl_errorstr,
				curl_easy_strerror(results->curl_result),
				sizeof(curl_errorstr));
#endif
		return HTTP_ERROR;
	}
}

int run_one_slot(struct active_request_slot *slot,
		 struct slot_results *results)
{
	slot->results = results;
	if (!start_active_slot(slot)) {
		snprintf(curl_errorstr, sizeof(curl_errorstr),
			 "failed to start HTTP request");
		return HTTP_START_FAILED;
	}

	run_active_slot(slot);
	return handle_curl_result(results);
}

struct curl_slist *http_copy_default_headers(void)
{
	struct curl_slist *headers = NULL, *h;

	for (h = extra_http_headers; h; h = h->next)
		headers = curl_slist_append(headers, h->data);

	return headers;
}

static CURLcode curlinfo_strbuf(CURL *curl, CURLINFO info, struct strbuf *buf)
{
	char *ptr;
	CURLcode ret;

	strbuf_reset(buf);
	ret = curl_easy_getinfo(curl, info, &ptr);
	if (!ret && ptr)
		strbuf_addstr(buf, ptr);
	return ret;
}

/*
 * Check for and extract a content-type parameter. "raw"
 * should be positioned at the start of the potential
 * parameter, with any whitespace already removed.
 *
 * "name" is the name of the parameter. The value is appended
 * to "out".
 */
static int extract_param(const char *raw, const char *name,
			 struct strbuf *out)
{
	size_t len = strlen(name);

	if (strncasecmp(raw, name, len))
		return -1;
	raw += len;

	if (*raw != '=')
		return -1;
	raw++;

	while (*raw && !isspace(*raw) && *raw != ';')
		strbuf_addch(out, *raw++);
	return 0;
}

/*
 * Extract a normalized version of the content type, with any
 * spaces suppressed, all letters lowercased, and no trailing ";"
 * or parameters.
 *
 * Note that we will silently remove even invalid whitespace. For
 * example, "text / plain" is specifically forbidden by RFC 2616,
 * but "text/plain" is the only reasonable output, and this keeps
 * our code simple.
 *
 * If the "charset" argument is not NULL, store the value of any
 * charset parameter there.
 *
 * Example:
 *   "TEXT/PLAIN; charset=utf-8" -> "text/plain", "utf-8"
 *   "text / plain" -> "text/plain"
 */
static void extract_content_type(struct strbuf *raw, struct strbuf *type,
				 struct strbuf *charset)
{
	const char *p;

	strbuf_reset(type);
	strbuf_grow(type, raw->len);
	for (p = raw->buf; *p; p++) {
		if (isspace(*p))
			continue;
		if (*p == ';') {
			p++;
			break;
		}
		strbuf_addch(type, tolower(*p));
	}

	if (!charset)
		return;

	strbuf_reset(charset);
	while (*p) {
		while (isspace(*p) || *p == ';')
			p++;
		if (!extract_param(p, "charset", charset))
			return;
		while (*p && !isspace(*p))
			p++;
	}

	if (!charset->len && starts_with(type->buf, "text/"))
		strbuf_addstr(charset, "ISO-8859-1");
}

static void write_accept_language(struct strbuf *buf)
{
	/*
	 * MAX_DECIMAL_PLACES must not be larger than 3. If it is larger than
	 * that, q-value will be smaller than 0.001, the minimum q-value the
	 * HTTP specification allows. See
	 * http://tools.ietf.org/html/rfc7231#section-5.3.1 for q-value.
	 */
	const int MAX_DECIMAL_PLACES = 3;
	const int MAX_LANGUAGE_TAGS = 1000;
	const int MAX_ACCEPT_LANGUAGE_HEADER_SIZE = 4000;
	char **language_tags = NULL;
	int num_langs = 0;
	const char *s = get_preferred_languages();
	int i;
	struct strbuf tag = STRBUF_INIT;

	/* Don't add Accept-Language header if no language is preferred. */
	if (!s)
		return;

	/*
	 * Split the colon-separated string of preferred languages into
	 * language_tags array.
	 */
	do {
		/* collect language tag */
		for (; *s && (isalnum(*s) || *s == '_'); s++)
			strbuf_addch(&tag, *s == '_' ? '-' : *s);

		/* skip .codeset, @modifier and any other unnecessary parts */
		while (*s && *s != ':')
			s++;

		if (tag.len) {
			num_langs++;
			REALLOC_ARRAY(language_tags, num_langs);
			language_tags[num_langs - 1] = strbuf_detach(&tag, NULL);
			if (num_langs >= MAX_LANGUAGE_TAGS - 1) /* -1 for '*' */
				break;
		}
	} while (*s++);

	/* write Accept-Language header into buf */
	if (num_langs) {
		int last_buf_len = 0;
		int max_q;
		int decimal_places;
		char q_format[32];

		/* add '*' */
		REALLOC_ARRAY(language_tags, num_langs + 1);
		language_tags[num_langs++] = "*"; /* it's OK; this won't be freed */

		/* compute decimal_places */
		for (max_q = 1, decimal_places = 0;
		     max_q < num_langs && decimal_places <= MAX_DECIMAL_PLACES;
		     decimal_places++, max_q *= 10)
			;

		xsnprintf(q_format, sizeof(q_format), ";q=0.%%0%dd", decimal_places);

		strbuf_addstr(buf, "Accept-Language: ");

		for (i = 0; i < num_langs; i++) {
			if (i > 0)
				strbuf_addstr(buf, ", ");

			strbuf_addstr(buf, language_tags[i]);

			if (i > 0)
				strbuf_addf(buf, q_format, max_q - i);

			if (buf->len > MAX_ACCEPT_LANGUAGE_HEADER_SIZE) {
				strbuf_remove(buf, last_buf_len, buf->len - last_buf_len);
				break;
			}

			last_buf_len = buf->len;
		}
	}

	/* free language tags -- last one is a static '*' */
	for (i = 0; i < num_langs - 1; i++)
		free(language_tags[i]);
	free(language_tags);
}

/*
 * Get an Accept-Language header which indicates user's preferred languages.
 *
 * Examples:
 *   LANGUAGE= -> ""
 *   LANGUAGE=ko:en -> "Accept-Language: ko, en; q=0.9, *; q=0.1"
 *   LANGUAGE=ko_KR.UTF-8:sr@latin -> "Accept-Language: ko-KR, sr; q=0.9, *; q=0.1"
 *   LANGUAGE=ko LANG=en_US.UTF-8 -> "Accept-Language: ko, *; q=0.1"
 *   LANGUAGE= LANG=en_US.UTF-8 -> "Accept-Language: en-US, *; q=0.1"
 *   LANGUAGE= LANG=C -> ""
 */
static const char *get_accept_language(void)
{
	if (!cached_accept_language) {
		struct strbuf buf = STRBUF_INIT;
		write_accept_language(&buf);
		if (buf.len > 0)
			cached_accept_language = strbuf_detach(&buf, NULL);
	}

	return cached_accept_language;
}

static void http_opt_request_remainder(CURL *curl, off_t pos)
{
	char buf[128];
	xsnprintf(buf, sizeof(buf), "%"PRIuMAX"-", (uintmax_t)pos);
	curl_easy_setopt(curl, CURLOPT_RANGE, buf);
}

/* http_request() targets */
#define HTTP_REQUEST_STRBUF	0
#define HTTP_REQUEST_FILE	1

static int http_request(const char *url,
			void *result, int target,
			const struct http_get_options *options)
{
	struct active_request_slot *slot;
	struct slot_results results;
	struct curl_slist *headers = http_copy_default_headers();
	struct strbuf buf = STRBUF_INIT;
	const char *accept_language;
	int ret;

	slot = get_active_slot();
	curl_easy_setopt(slot->curl, CURLOPT_HTTPGET, 1);

	if (result == NULL) {
		curl_easy_setopt(slot->curl, CURLOPT_NOBODY, 1);
	} else {
		curl_easy_setopt(slot->curl, CURLOPT_NOBODY, 0);
		curl_easy_setopt(slot->curl, CURLOPT_FILE, result);

		if (target == HTTP_REQUEST_FILE) {
			off_t posn = ftello(result);
			curl_easy_setopt(slot->curl, CURLOPT_WRITEFUNCTION,
					 fwrite);
			if (posn > 0)
				http_opt_request_remainder(slot->curl, posn);
		} else
			curl_easy_setopt(slot->curl, CURLOPT_WRITEFUNCTION,
					 fwrite_buffer);
	}

	accept_language = get_accept_language();

	if (accept_language)
		headers = curl_slist_append(headers, accept_language);

	strbuf_addstr(&buf, "Pragma:");
	if (options && options->no_cache)
		strbuf_addstr(&buf, " no-cache");
	if (options && options->keep_error)
		curl_easy_setopt(slot->curl, CURLOPT_FAILONERROR, 0);

	headers = curl_slist_append(headers, buf.buf);

	curl_easy_setopt(slot->curl, CURLOPT_URL, url);
	curl_easy_setopt(slot->curl, CURLOPT_HTTPHEADER, headers);
	curl_easy_setopt(slot->curl, CURLOPT_ENCODING, "gzip");

	ret = run_one_slot(slot, &results);

	if (options && options->content_type) {
		struct strbuf raw = STRBUF_INIT;
		curlinfo_strbuf(slot->curl, CURLINFO_CONTENT_TYPE, &raw);
		extract_content_type(&raw, options->content_type,
				     options->charset);
		strbuf_release(&raw);
	}

	if (options && options->effective_url)
		curlinfo_strbuf(slot->curl, CURLINFO_EFFECTIVE_URL,
				options->effective_url);

	curl_slist_free_all(headers);
	strbuf_release(&buf);

	return ret;
}

/*
 * Update the "base" url to a more appropriate value, as deduced by
 * redirects seen when requesting a URL starting with "url".
 *
 * The "asked" parameter is a URL that we asked curl to access, and must begin
 * with "base".
 *
 * The "got" parameter is the URL that curl reported to us as where we ended
 * up.
 *
 * Returns 1 if we updated the base url, 0 otherwise.
 *
 * Our basic strategy is to compare "base" and "asked" to find the bits
 * specific to our request. We then strip those bits off of "got" to yield the
 * new base. So for example, if our base is "http://example.com/foo.git",
 * and we ask for "http://example.com/foo.git/info/refs", we might end up
 * with "https://other.example.com/foo.git/info/refs". We would want the
 * new URL to become "https://other.example.com/foo.git".
 *
 * Note that this assumes a sane redirect scheme. It's entirely possible
 * in the example above to end up at a URL that does not even end in
 * "info/refs".  In such a case we simply punt, as there is not much we can
 * do (and such a scheme is unlikely to represent a real git repository,
 * which means we are likely about to abort anyway).
 */
static int update_url_from_redirect(struct strbuf *base,
				    const char *asked,
				    const struct strbuf *got)
{
	const char *tail;
	size_t tail_len;

	if (!strcmp(asked, got->buf))
		return 0;

	if (!skip_prefix(asked, base->buf, &tail))
		die("BUG: update_url_from_redirect: %s is not a superset of %s",
		    asked, base->buf);

	tail_len = strlen(tail);

	if (got->len < tail_len ||
	    strcmp(tail, got->buf + got->len - tail_len))
		return 0; /* insane redirect scheme */

	strbuf_reset(base);
	strbuf_add(base, got->buf, got->len - tail_len);
	return 1;
}

static int http_request_reauth(const char *url,
			       void *result, int target,
			       struct http_get_options *options)
{
	int ret = http_request(url, result, target, options);

	if (options && options->effective_url && options->base_url) {
		if (update_url_from_redirect(options->base_url,
					     url, options->effective_url)) {
			credential_from_url(&http_auth, options->base_url->buf);
			url = options->effective_url->buf;
		}
	}

	if (ret != HTTP_REAUTH)
		return ret;

	/*
	 * If we are using KEEP_ERROR, the previous request may have
	 * put cruft into our output stream; we should clear it out before
	 * making our next request. We only know how to do this for
	 * the strbuf case, but that is enough to satisfy current callers.
	 */
	if (options && options->keep_error) {
		switch (target) {
		case HTTP_REQUEST_STRBUF:
			strbuf_reset(result);
			break;
		default:
			die("BUG: HTTP_KEEP_ERROR is only supported with strbufs");
		}
	}

	credential_fill(&http_auth);

	return http_request(url, result, target, options);
}

int http_get_strbuf(const char *url,
		    struct strbuf *result,
		    struct http_get_options *options)
{
	return http_request_reauth(url, result, HTTP_REQUEST_STRBUF, options);
}

/*
 * Downloads a URL and stores the result in the given file.
 *
 * If a previous interrupted download is detected (i.e. a previous temporary
 * file is still around) the download is resumed.
 */
static int http_get_file(const char *url, const char *filename,
			 struct http_get_options *options)
{
	int ret;
	struct strbuf tmpfile = STRBUF_INIT;
	FILE *result;

	strbuf_addf(&tmpfile, "%s.temp", filename);
	result = fopen(tmpfile.buf, "a");
	if (!result) {
		error("Unable to open local file %s", tmpfile.buf);
		ret = HTTP_ERROR;
		goto cleanup;
	}

	ret = http_request_reauth(url, result, HTTP_REQUEST_FILE, options);
	fclose(result);

	if (ret == HTTP_OK && finalize_object_file(tmpfile.buf, filename))
		ret = HTTP_ERROR;
cleanup:
	strbuf_release(&tmpfile);
	return ret;
}

int http_fetch_ref(const char *base, struct ref *ref)
{
	struct http_get_options options = {0};
	char *url;
	struct strbuf buffer = STRBUF_INIT;
	int ret = -1;

	options.no_cache = 1;

	url = quote_ref_url(base, ref->name);
	if (http_get_strbuf(url, &buffer, &options) == HTTP_OK) {
		strbuf_rtrim(&buffer);
		if (buffer.len == 40)
			ret = get_oid_hex(buffer.buf, &ref->old_oid);
		else if (starts_with(buffer.buf, "ref: ")) {
			ref->symref = xstrdup(buffer.buf + 5);
			ret = 0;
		}
	}

	strbuf_release(&buffer);
	free(url);
	return ret;
}

/* Helpers for fetching packs */
static char *fetch_pack_index(unsigned char *sha1, const char *base_url)
{
	char *url, *tmp;
	struct strbuf buf = STRBUF_INIT;

	if (http_is_verbose)
		fprintf(stderr, "Getting index for pack %s\n", sha1_to_hex(sha1));

	end_url_with_slash(&buf, base_url);
	strbuf_addf(&buf, "objects/pack/pack-%s.idx", sha1_to_hex(sha1));
	url = strbuf_detach(&buf, NULL);

	strbuf_addf(&buf, "%s.temp", sha1_pack_index_name(sha1));
	tmp = strbuf_detach(&buf, NULL);

	if (http_get_file(url, tmp, NULL) != HTTP_OK) {
		error("Unable to get pack index %s", url);
		free(tmp);
		tmp = NULL;
	}

	free(url);
	return tmp;
}

static int fetch_and_setup_pack_index(struct packed_git **packs_head,
	unsigned char *sha1, const char *base_url)
{
	struct packed_git *new_pack;
	char *tmp_idx = NULL;
	int ret;

	if (has_pack_index(sha1)) {
		new_pack = parse_pack_index(sha1, sha1_pack_index_name(sha1));
		if (!new_pack)
			return -1; /* parse_pack_index() already issued error message */
		goto add_pack;
	}

	tmp_idx = fetch_pack_index(sha1, base_url);
	if (!tmp_idx)
		return -1;

	new_pack = parse_pack_index(sha1, tmp_idx);
	if (!new_pack) {
		unlink(tmp_idx);
		free(tmp_idx);

		return -1; /* parse_pack_index() already issued error message */
	}

	ret = verify_pack_index(new_pack);
	if (!ret) {
		close_pack_index(new_pack);
		ret = finalize_object_file(tmp_idx, sha1_pack_index_name(sha1));
	}
	free(tmp_idx);
	if (ret)
		return -1;

add_pack:
	new_pack->next = *packs_head;
	*packs_head = new_pack;
	return 0;
}

int http_get_info_packs(const char *base_url, struct packed_git **packs_head)
{
	struct http_get_options options = {0};
	int ret = 0, i = 0;
	char *url, *data;
	struct strbuf buf = STRBUF_INIT;
	unsigned char sha1[20];

	end_url_with_slash(&buf, base_url);
	strbuf_addstr(&buf, "objects/info/packs");
	url = strbuf_detach(&buf, NULL);

	options.no_cache = 1;
	ret = http_get_strbuf(url, &buf, &options);
	if (ret != HTTP_OK)
		goto cleanup;

	data = buf.buf;
	while (i < buf.len) {
		switch (data[i]) {
		case 'P':
			i++;
			if (i + 52 <= buf.len &&
			    starts_with(data + i, " pack-") &&
			    starts_with(data + i + 46, ".pack\n")) {
				get_sha1_hex(data + i + 6, sha1);
				fetch_and_setup_pack_index(packs_head, sha1,
						      base_url);
				i += 51;
				break;
			}
		default:
			while (i < buf.len && data[i] != '\n')
				i++;
		}
		i++;
	}

cleanup:
	free(url);
	return ret;
}

void release_http_pack_request(struct http_pack_request *preq)
{
	if (preq->packfile != NULL) {
		fclose(preq->packfile);
		preq->packfile = NULL;
	}
	preq->slot = NULL;
	free(preq->url);
	free(preq);
}

int finish_http_pack_request(struct http_pack_request *preq)
{
	struct packed_git **lst;
	struct packed_git *p = preq->target;
	char *tmp_idx;
	size_t len;
	struct child_process ip = CHILD_PROCESS_INIT;
	const char *ip_argv[8];

	close_pack_index(p);

	fclose(preq->packfile);
	preq->packfile = NULL;

	lst = preq->lst;
	while (*lst != p)
		lst = &((*lst)->next);
	*lst = (*lst)->next;

	if (!strip_suffix(preq->tmpfile, ".pack.temp", &len))
		die("BUG: pack tmpfile does not end in .pack.temp?");
	tmp_idx = xstrfmt("%.*s.idx.temp", (int)len, preq->tmpfile);

	ip_argv[0] = "index-pack";
	ip_argv[1] = "-o";
	ip_argv[2] = tmp_idx;
	ip_argv[3] = preq->tmpfile;
	ip_argv[4] = NULL;

	ip.argv = ip_argv;
	ip.git_cmd = 1;
	ip.no_stdin = 1;
	ip.no_stdout = 1;

	if (run_command(&ip)) {
		unlink(preq->tmpfile);
		unlink(tmp_idx);
		free(tmp_idx);
		return -1;
	}

	unlink(sha1_pack_index_name(p->sha1));

	if (finalize_object_file(preq->tmpfile, sha1_pack_name(p->sha1))
	 || finalize_object_file(tmp_idx, sha1_pack_index_name(p->sha1))) {
		free(tmp_idx);
		return -1;
	}

	install_packed_git(p);
	free(tmp_idx);
	return 0;
}

struct http_pack_request *new_http_pack_request(
	struct packed_git *target, const char *base_url)
{
	off_t prev_posn = 0;
	struct strbuf buf = STRBUF_INIT;
	struct http_pack_request *preq;

	preq = xcalloc(1, sizeof(*preq));
	preq->target = target;

	end_url_with_slash(&buf, base_url);
	strbuf_addf(&buf, "objects/pack/pack-%s.pack",
		sha1_to_hex(target->sha1));
	preq->url = strbuf_detach(&buf, NULL);

	snprintf(preq->tmpfile, sizeof(preq->tmpfile), "%s.temp",
		sha1_pack_name(target->sha1));
	preq->packfile = fopen(preq->tmpfile, "a");
	if (!preq->packfile) {
		error("Unable to open local file %s for pack",
		      preq->tmpfile);
		goto abort;
	}

	preq->slot = get_active_slot();
	curl_easy_setopt(preq->slot->curl, CURLOPT_FILE, preq->packfile);
	curl_easy_setopt(preq->slot->curl, CURLOPT_WRITEFUNCTION, fwrite);
	curl_easy_setopt(preq->slot->curl, CURLOPT_URL, preq->url);
	curl_easy_setopt(preq->slot->curl, CURLOPT_HTTPHEADER,
		no_pragma_header);

	/*
	 * If there is data present from a previous transfer attempt,
	 * resume where it left off
	 */
	prev_posn = ftello(preq->packfile);
	if (prev_posn>0) {
		if (http_is_verbose)
			fprintf(stderr,
				"Resuming fetch of pack %s at byte %"PRIuMAX"\n",
				sha1_to_hex(target->sha1), (uintmax_t)prev_posn);
		http_opt_request_remainder(preq->slot->curl, prev_posn);
	}

	return preq;

abort:
	free(preq->url);
	free(preq);
	return NULL;
}

/* Helpers for fetching objects (loose) */
static size_t fwrite_sha1_file(char *ptr, size_t eltsize, size_t nmemb,
			       void *data)
{
	unsigned char expn[4096];
	size_t size = eltsize * nmemb;
	int posn = 0;
	struct http_object_request *freq =
		(struct http_object_request *)data;
	do {
		ssize_t retval = xwrite(freq->localfile,
					(char *) ptr + posn, size - posn);
		if (retval < 0)
			return posn;
		posn += retval;
	} while (posn < size);

	freq->stream.avail_in = size;
	freq->stream.next_in = (void *)ptr;
	do {
		freq->stream.next_out = expn;
		freq->stream.avail_out = sizeof(expn);
		freq->zret = git_inflate(&freq->stream, Z_SYNC_FLUSH);
		git_SHA1_Update(&freq->c, expn,
				sizeof(expn) - freq->stream.avail_out);
	} while (freq->stream.avail_in && freq->zret == Z_OK);
	return size;
}

struct http_object_request *new_http_object_request(const char *base_url,
	unsigned char *sha1)
{
	char *hex = sha1_to_hex(sha1);
	const char *filename;
	char prevfile[PATH_MAX];
	int prevlocal;
	char prev_buf[PREV_BUF_SIZE];
	ssize_t prev_read = 0;
	off_t prev_posn = 0;
	struct http_object_request *freq;

	freq = xcalloc(1, sizeof(*freq));
	hashcpy(freq->sha1, sha1);
	freq->localfile = -1;

	filename = sha1_file_name(sha1);
	snprintf(freq->tmpfile, sizeof(freq->tmpfile),
		 "%s.temp", filename);

	snprintf(prevfile, sizeof(prevfile), "%s.prev", filename);
	unlink_or_warn(prevfile);
	rename(freq->tmpfile, prevfile);
	unlink_or_warn(freq->tmpfile);

	if (freq->localfile != -1)
		error("fd leakage in start: %d", freq->localfile);
	freq->localfile = open(freq->tmpfile,
			       O_WRONLY | O_CREAT | O_EXCL, 0666);
	/*
	 * This could have failed due to the "lazy directory creation";
	 * try to mkdir the last path component.
	 */
	if (freq->localfile < 0 && errno == ENOENT) {
		char *dir = strrchr(freq->tmpfile, '/');
		if (dir) {
			*dir = 0;
			mkdir(freq->tmpfile, 0777);
			*dir = '/';
		}
		freq->localfile = open(freq->tmpfile,
				       O_WRONLY | O_CREAT | O_EXCL, 0666);
	}

	if (freq->localfile < 0) {
		error_errno("Couldn't create temporary file %s", freq->tmpfile);
		goto abort;
	}

	git_inflate_init(&freq->stream);

	git_SHA1_Init(&freq->c);

	freq->url = get_remote_object_url(base_url, hex, 0);

	/*
	 * If a previous temp file is present, process what was already
	 * fetched.
	 */
	prevlocal = open(prevfile, O_RDONLY);
	if (prevlocal != -1) {
		do {
			prev_read = xread(prevlocal, prev_buf, PREV_BUF_SIZE);
			if (prev_read>0) {
				if (fwrite_sha1_file(prev_buf,
						     1,
						     prev_read,
						     freq) == prev_read) {
					prev_posn += prev_read;
				} else {
					prev_read = -1;
				}
			}
		} while (prev_read > 0);
		close(prevlocal);
	}
	unlink_or_warn(prevfile);

	/*
	 * Reset inflate/SHA1 if there was an error reading the previous temp
	 * file; also rewind to the beginning of the local file.
	 */
	if (prev_read == -1) {
		memset(&freq->stream, 0, sizeof(freq->stream));
		git_inflate_init(&freq->stream);
		git_SHA1_Init(&freq->c);
		if (prev_posn>0) {
			prev_posn = 0;
			lseek(freq->localfile, 0, SEEK_SET);
			if (ftruncate(freq->localfile, 0) < 0) {
				error_errno("Couldn't truncate temporary file %s",
					    freq->tmpfile);
				goto abort;
			}
		}
	}

	freq->slot = get_active_slot();

	curl_easy_setopt(freq->slot->curl, CURLOPT_FILE, freq);
	curl_easy_setopt(freq->slot->curl, CURLOPT_WRITEFUNCTION, fwrite_sha1_file);
	curl_easy_setopt(freq->slot->curl, CURLOPT_ERRORBUFFER, freq->errorstr);
	curl_easy_setopt(freq->slot->curl, CURLOPT_URL, freq->url);
	curl_easy_setopt(freq->slot->curl, CURLOPT_HTTPHEADER, no_pragma_header);

	/*
	 * If we have successfully processed data from a previous fetch
	 * attempt, only fetch the data we don't already have.
	 */
	if (prev_posn>0) {
		if (http_is_verbose)
			fprintf(stderr,
				"Resuming fetch of object %s at byte %"PRIuMAX"\n",
				hex, (uintmax_t)prev_posn);
		http_opt_request_remainder(freq->slot->curl, prev_posn);
	}

	return freq;

abort:
	free(freq->url);
	free(freq);
	return NULL;
}

void process_http_object_request(struct http_object_request *freq)
{
	if (freq->slot == NULL)
		return;
	freq->curl_result = freq->slot->curl_result;
	freq->http_code = freq->slot->http_code;
	freq->slot = NULL;
}

int finish_http_object_request(struct http_object_request *freq)
{
	struct stat st;

	close(freq->localfile);
	freq->localfile = -1;

	process_http_object_request(freq);

	if (freq->http_code == 416) {
		warning("requested range invalid; we may already have all the data.");
	} else if (freq->curl_result != CURLE_OK) {
		if (stat(freq->tmpfile, &st) == 0)
			if (st.st_size == 0)
				unlink_or_warn(freq->tmpfile);
		return -1;
	}

	git_inflate_end(&freq->stream);
	git_SHA1_Final(freq->real_sha1, &freq->c);
	if (freq->zret != Z_STREAM_END) {
		unlink_or_warn(freq->tmpfile);
		return -1;
	}
	if (hashcmp(freq->sha1, freq->real_sha1)) {
		unlink_or_warn(freq->tmpfile);
		return -1;
	}
	freq->rename =
		finalize_object_file(freq->tmpfile, sha1_file_name(freq->sha1));

	return freq->rename;
}

void abort_http_object_request(struct http_object_request *freq)
{
	unlink_or_warn(freq->tmpfile);

	release_http_object_request(freq);
}

void release_http_object_request(struct http_object_request *freq)
{
	if (freq->localfile != -1) {
		close(freq->localfile);
		freq->localfile = -1;
	}
	if (freq->url != NULL) {
		free(freq->url);
		freq->url = NULL;
	}
	if (freq->slot != NULL) {
		freq->slot->callback_func = NULL;
		freq->slot->callback_data = NULL;
		release_active_slot(freq->slot);
		freq->slot = NULL;
	}
}
