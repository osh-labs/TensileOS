/* Host tests for tensile_upload core (URL, auth, classification, PROPFIND, backoff, planner). */
#include "tinytest.h"
#include "tu_core.h"

#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#define BASE "https://nx80213.your-storageshare.de/remote.php/dav/files/tensile-hmi/TensileOS/"

/* ------------------------------------------------------------------------ */
static void test_url_builder(void)
{
    char out[TU_URL_MAX];
    TT_EQ_INT(tu_url_build(BASE, "Tests/2026-10-14/name.csv", out, sizeof out), TU_OK);
    TT_EQ_STR(out, BASE "Tests/2026-10-14/name.csv");

    /* base without trailing slash, rel with leading slash and trailing slash */
    TT_EQ_INT(tu_url_build("https://h/dav/TensileOS", "/Tests/2026-10-14/", out, sizeof out), TU_OK);
    TT_EQ_STR(out, "https://h/dav/TensileOS/Tests/2026-10-14/");

    /* base itself */
    TT_EQ_INT(tu_url_build(BASE, "", out, sizeof out), TU_OK);
    TT_EQ_STR(out, BASE);
    TT_EQ_INT(tu_url_build(BASE, NULL, out, sizeof out), TU_OK);
    TT_EQ_STR(out, BASE);

    /* spaces, reserved chars, percent, unicode (UTF-8 "é" = C3 A9, "µ" = C2 B5) */
    TT_EQ_INT(tu_path_encode("Tests/Pull Test #1?.csv", out, sizeof out), TU_OK);
    TT_EQ_STR(out, "Tests/Pull%20Test%20%231%3F.csv");
    TT_EQ_INT(tu_path_encode("100%/a+b&c=d;e,f@g:h", out, sizeof out), TU_OK);
    TT_EQ_STR(out, "100%25/a%2Bb%26c%3Dd%3Be%2Cf%40g%3Ah");
    TT_EQ_INT(tu_path_encode("caf\xC3\xA9/5\xC2\xB5m.csv", out, sizeof out), TU_OK);
    TT_EQ_STR(out, "caf%C3%A9/5%C2%B5m.csv");
    TT_EQ_INT(tu_path_encode("keep-this_._~ok", out, sizeof out), TU_OK);
    TT_EQ_STR(out, "keep-this_._~ok");
    /* double slashes collapse */
    TT_EQ_INT(tu_path_encode("a//b///c", out, sizeof out), TU_OK);
    TT_EQ_STR(out, "a/b/c");

    /* traversal and bad chars rejected, output cleared */
    TT_EQ_INT(tu_url_build(BASE, "Tests/../../admin/x", out, sizeof out), TU_ERR_PATH);
    TT_EQ_STR(out, "");
    TT_EQ_INT(tu_path_encode("..", out, sizeof out), TU_ERR_PATH);
    TT_EQ_INT(tu_path_encode("a/./b", out, sizeof out), TU_ERR_PATH);
    TT_EQ_INT(tu_path_encode("a/b\\c", out, sizeof out), TU_ERR_PATH);
    TT_EQ_INT(tu_path_encode("a\nb", out, sizeof out), TU_ERR_PATH);
    /* "..." and "..x" are legal names */
    TT_EQ_INT(tu_path_encode(".../..x", out, sizeof out), TU_OK);
    TT_EQ_STR(out, ".../..x");

    /* overflow */
    char small[12];
    TT_EQ_INT(tu_path_encode("Tests/very long name", small, sizeof small), TU_ERR_OVERFLOW);
    TT_CHECK(strlen(small) < sizeof small);
    TT_EQ_INT(tu_url_build(BASE, "x", small, sizeof small), TU_ERR_OVERFLOW);

    /* decode round trip */
    char dec[64];
    TT_EQ_INT(tu_percent_decode("Pull%20Test%20%C3%A9%231.csv", dec, sizeof dec), TU_OK);
    TT_EQ_STR(dec, "Pull Test \xC3\xA9#1.csv");
    TT_EQ_INT(tu_percent_decode("bad%zzescape%4", dec, sizeof dec), TU_OK);
    TT_EQ_STR(dec, "bad%zzescape%4");
}

/* ------------------------------------------------------------------------ */
static void test_auth(void)
{
    char out[TU_AUTH_HDR_MAX];
    /* RFC 7617 example */
    TT_EQ_INT(tu_basic_auth_header("Aladdin", "open sesame", out, sizeof out), TU_OK);
    TT_EQ_STR(out, "Basic QWxhZGRpbjpvcGVuIHNlc2FtZQ==");
    /* passwords may contain ':' */
    TT_EQ_INT(tu_basic_auth_header("tensile-hmi", "a:b", out, sizeof out), TU_OK);
    TT_EQ_STR(out, "Basic dGVuc2lsZS1obWk6YTpi");
    /* user with ':' rejected */
    TT_EQ_INT(tu_basic_auth_header("bad:user", "x", out, sizeof out), TU_ERR_ARG);
    TT_EQ_STR(out, "");
    TT_EQ_INT(tu_basic_auth_header("", "x", out, sizeof out), TU_ERR_ARG);
    /* overflow zeroes the buffer */
    char small[10];
    memset(small, 'Z', sizeof small);
    TT_EQ_INT(tu_basic_auth_header("tensile-hmi", "Abcde-Fghij-Klmno-Pqrst-Uvwxy", small, sizeof small),
              TU_ERR_OVERFLOW);
    bool all_zero = true;
    for (size_t i = 0; i < sizeof small; i++) all_zero &= (small[i] == 0);
    TT_CHECK(all_zero);

    /* base64 RFC 4648 vectors */
    const char *vin[] = {"", "f", "fo", "foo", "foob", "fooba", "foobar"};
    const char *vout[] = {"", "Zg==", "Zm8=", "Zm9v", "Zm9vYg==", "Zm9vYmE=", "Zm9vYmFy"};
    for (int i = 0; i < 7; i++) {
        char b[16];
        int n = tu_base64_encode((const uint8_t *)vin[i], strlen(vin[i]), b, sizeof b);
        TT_EQ_INT(n, (int)strlen(vout[i]));
        TT_EQ_STR(b, vout[i]);
    }
    char b5[5];
    TT_EQ_INT(tu_base64_encode((const uint8_t *)"foob", 4, b5, sizeof b5), TU_ERR_OVERFLOW);

    char secret[16] = "app-password";
    tu_secure_zero(secret, sizeof secret);
    TT_EQ_INT(secret[0], 0);
    TT_EQ_INT(secret[11], 0);
}

/* ------------------------------------------------------------------------ */
static void test_http_date(void)
{
    int64_t t = 0;
    /* RFC 9110 examples: 784111777 = Sun, 06 Nov 1994 08:49:37 GMT */
    TT_EQ_INT(tu_parse_http_date("Sun, 06 Nov 1994 08:49:37 GMT", &t), TU_OK);
    TT_EQ_INT(t, 784111777);
    TT_EQ_INT(tu_parse_http_date("Sunday, 06-Nov-94 08:49:37 GMT", &t), TU_OK);
    TT_EQ_INT(t, 784111777);
    TT_EQ_INT(tu_parse_http_date("Sun Nov  6 08:49:37 1994", &t), TU_OK);
    TT_EQ_INT(t, 784111777);
    TT_EQ_INT(tu_parse_http_date("Wed, 14 Oct 2026 12:00:00 GMT", &t), TU_OK);
    TT_EQ_INT(t, 1791979200);
    TT_EQ_INT(tu_parse_http_date("Thu, 29 Feb 2024 23:59:59 GMT", &t), TU_OK);
    TT_EQ_INT(t, 1709251199);
    TT_EQ_INT(tu_parse_http_date("garbage", &t), TU_ERR_PARSE);
    TT_EQ_INT(tu_parse_http_date("Sun, 06 Foo 1994 08:49:37 GMT", &t), TU_ERR_PARSE);
    TT_EQ_INT(tu_parse_http_date("Sun, 06 Nov 1994 08:49 GMT", &t), TU_ERR_PARSE);
}

static void test_retry_after(void)
{
    const int64_t now = 1791979200; /* Wed, 14 Oct 2026 12:00:00 GMT */
    TT_EQ_INT(tu_parse_retry_after("120", now), 120);
    TT_EQ_INT(tu_parse_retry_after("  0 ", now), 0);
    TT_EQ_INT(tu_parse_retry_after("99999999999", now), TU_RETRY_AFTER_MAX_S);
    TT_EQ_INT(tu_parse_retry_after("12abc", now), -1);
    TT_EQ_INT(tu_parse_retry_after("", now), -1);
    TT_EQ_INT(tu_parse_retry_after(NULL, now), -1);
    TT_EQ_INT(tu_parse_retry_after("Wed, 14 Oct 2026 12:01:30 GMT", now), 90);
    TT_EQ_INT(tu_parse_retry_after("Wed, 14 Oct 2026 11:00:00 GMT", now), 0); /* in the past */
    TT_EQ_INT(tu_parse_retry_after("Wed, 14 Oct 2026 12:01:30 GMT", 0), -1);  /* clock not synced */
}

static void test_classify(void)
{
    const int64_t now = 1791979200;
    tu_class_t c;
    /* PROPFIND */
    TT_EQ_INT(tu_classify(TU_METHOD_PROPFIND, 207, NULL, now).result, TU_RES_OK);
    TT_EQ_INT(tu_classify(TU_METHOD_PROPFIND, 200, NULL, now).result, TU_RES_PERMANENT_ERROR); /* captive portal */
    TT_EQ_INT(tu_classify(TU_METHOD_PROPFIND, 404, NULL, now).result, TU_RES_NOT_FOUND);
    TT_EQ_INT(tu_classify(TU_METHOD_PROPFIND, 409, NULL, now).result, TU_RES_PERMANENT_ERROR);
    /* MKCOL */
    TT_EQ_INT(tu_classify(TU_METHOD_MKCOL, 201, NULL, now).result, TU_RES_CREATED);
    TT_EQ_INT(tu_classify(TU_METHOD_MKCOL, 405, NULL, now).result, TU_RES_ALREADY_EXISTS);
    TT_EQ_INT(tu_classify(TU_METHOD_MKCOL, 409, NULL, now).result, TU_RES_NOT_FOUND);
    TT_EQ_INT(tu_classify(TU_METHOD_MKCOL, 200, NULL, now).result, TU_RES_PERMANENT_ERROR);
    /* PUT */
    TT_EQ_INT(tu_classify(TU_METHOD_PUT, 201, NULL, now).result, TU_RES_CREATED);
    TT_EQ_INT(tu_classify(TU_METHOD_PUT, 204, NULL, now).result, TU_RES_OK);
    TT_EQ_INT(tu_classify(TU_METHOD_PUT, 200, NULL, now).result, TU_RES_OK);
    TT_EQ_INT(tu_classify(TU_METHOD_PUT, 412, NULL, now).result, TU_RES_PRECONDITION_FAILED);
    TT_EQ_INT(tu_classify(TU_METHOD_PUT, 405, NULL, now).result, TU_RES_PERMANENT_ERROR);
    TT_EQ_INT(tu_classify(TU_METHOD_PUT, 409, NULL, now).result, TU_RES_NOT_FOUND);
    TT_EQ_INT(tu_classify(TU_METHOD_PUT, 413, NULL, now).result, TU_RES_PERMANENT_ERROR);
    /* auth: never retry */
    c = tu_classify(TU_METHOD_PUT, 401, "60", now);
    TT_EQ_INT(c.result, TU_RES_AUTH_FAILED);
    TT_EQ_INT(c.retry_after_s, -1);
    TT_EQ_INT(tu_classify(TU_METHOD_PROPFIND, 403, NULL, now).result, TU_RES_AUTH_FAILED);
    TT_EQ_INT(tu_classify(TU_METHOD_MKCOL, 401, NULL, now).result, TU_RES_AUTH_FAILED);
    /* retry later */
    int retry[] = {408, 423, 425, 429, 500, 502, 503, 504, 507};
    for (size_t i = 0; i < sizeof retry / sizeof retry[0]; i++)
        TT_EQ_INT(tu_classify(TU_METHOD_PUT, retry[i], NULL, now).result, TU_RES_RETRY_LATER);
    c = tu_classify(TU_METHOD_PUT, 503, "30", now);
    TT_EQ_INT(c.retry_after_s, 30);
    TT_EQ_INT(c.http_status, 503);
    c = tu_classify(TU_METHOD_PUT, 429, "Wed, 14 Oct 2026 12:05:00 GMT", now);
    TT_EQ_INT(c.result, TU_RES_RETRY_LATER);
    TT_EQ_INT(c.retry_after_s, 300);
    c = tu_classify(TU_METHOD_PUT, 500, NULL, now);
    TT_EQ_INT(c.retry_after_s, -1);
    /* transport error */
    c = tu_classify(TU_METHOD_PUT, 0, NULL, now);
    TT_EQ_INT(c.result, TU_RES_RETRY_LATER);
    TT_EQ_INT(c.http_status, 0);
    TT_EQ_INT(tu_classify(TU_METHOD_PUT, -1, NULL, now).result, TU_RES_RETRY_LATER);
    /* permanent */
    TT_EQ_INT(tu_classify(TU_METHOD_PUT, 400, NULL, now).result, TU_RES_PERMANENT_ERROR);
    TT_EQ_INT(tu_classify(TU_METHOD_PUT, 501, NULL, now).result, TU_RES_PERMANENT_ERROR);
    TT_EQ_INT(tu_classify(TU_METHOD_PROPFIND, 301, NULL, now).result, TU_RES_PERMANENT_ERROR);
    TT_EQ_INT(tu_classify(TU_METHOD_PUT, 100, NULL, now).result, TU_RES_PERMANENT_ERROR);

    TT_CHECK(tu_result_is_success(TU_RES_ALREADY_EXISTS));
    TT_CHECK(!tu_result_is_success(TU_RES_PRECONDITION_FAILED));
    TT_EQ_STR(tu_result_name(TU_RES_AUTH_FAILED), "AUTH_FAILED");

    TT_EQ_INT(tu_parse_status_line("HTTP/1.1 207 Multi-Status"), 207);
    TT_EQ_INT(tu_parse_status_line("  HTTP/1.1 404 Not Found"), 404);
    TT_EQ_INT(tu_parse_status_line("HTTP/2 200"), 200);
    TT_EQ_INT(tu_parse_status_line("bogus"), 0);
}

/* ------------------------------------------------------------------------ */
/* Nextcloud (SabreDAV) 207 response to PROPFIND Depth: 1 on Tests/2026-10-13/,
 * modelled on the Nextcloud WebDAV docs (d:/oc:/nc: namespaces, quoted getetag
 * escaped as &quot;, absent getcontentlength reported in a 404 propstat for
 * collections). Sabre emits it on one line; split here for readability only. */
static const char NC_PROPFIND_207[] =
    "<?xml version=\"1.0\"?>\n"
    "<d:multistatus xmlns:d=\"DAV:\" xmlns:s=\"http://sabredav.org/ns\" xmlns:oc=\"http://owncloud.org/ns\" "
    "xmlns:nc=\"http://nextcloud.org/ns\">"
    "<d:response><d:href>/remote.php/dav/files/tensile-hmi/TensileOS/Tests/2026-10-13/</d:href>"
    "<d:propstat><d:prop><d:getlastmodified>Tue, 13 Oct 2026 14:02:11 GMT</d:getlastmodified>"
    "<d:resourcetype><d:collection/></d:resourcetype>"
    "<d:getetag>&quot;6706c1a3e1f0b&quot;</d:getetag><oc:size>6254</oc:size>"
    "<oc:permissions>RGDNVCK</oc:permissions></d:prop>"
    "<d:status>HTTP/1.1 200 OK</d:status></d:propstat>"
    "<d:propstat><d:prop><d:getcontentlength/></d:prop>"
    "<d:status>HTTP/1.1 404 Not Found</d:status></d:propstat></d:response>"
    "<d:response><d:href>/remote.php/dav/files/tensile-hmi/TensileOS/Tests/2026-10-13/"
    "Pull%20Test%20%C3%A9%231.csv</d:href>"
    "<d:propstat><d:prop><d:getlastmodified>Tue, 13 Oct 2026 14:02:11 GMT</d:getlastmodified>"
    "<d:getcontentlength>3127</d:getcontentlength><d:resourcetype/>"
    "<d:getetag>&quot;a3f5c0d1e2b7489f9c1d2e3f4a5b6c7d&quot;</d:getetag>"
    "<oc:fileid>18342</oc:fileid><nc:has-preview>false</nc:has-preview></d:prop>"
    "<d:status>HTTP/1.1 200 OK</d:status></d:propstat></d:response>"
    "<d:response><d:href>/remote.php/dav/files/tensile-hmi/TensileOS/Tests/2026-10-13/S_1_TAB5.csv</d:href>"
    "<d:propstat><d:prop><d:getlastmodified>Tue, 13 Oct 2026 14:05:40 GMT</d:getlastmodified>"
    "<d:getcontentlength>3127</d:getcontentlength><d:resourcetype/>"
    "<d:getetag>&quot;0b1c2d3e4f5061728394a5b6c7d8e9f0&quot;</d:getetag></d:prop>"
    "<d:status>HTTP/1.1 200 OK</d:status></d:propstat></d:response>"
    "</d:multistatus>\n";

static void test_propfind_nextcloud(void)
{
    tu_dav_entry_t e[4];
    size_t n = 99;
    TT_EQ_INT(tu_propfind_parse(NC_PROPFIND_207, strlen(NC_PROPFIND_207), e, 4, &n), TU_OK);
    TT_EQ_INT(n, 3);

    TT_EQ_STR(e[0].href, "/remote.php/dav/files/tensile-hmi/TensileOS/Tests/2026-10-13/");
    TT_EQ_INT(e[0].status, 200);
    TT_CHECK(e[0].is_collection);
    TT_CHECK(e[0].has_etag);
    TT_EQ_STR(e[0].etag, "\"6706c1a3e1f0b\"");
    TT_EQ_INT(e[0].content_length, -1);

    TT_EQ_STR(e[1].href, "/remote.php/dav/files/tensile-hmi/TensileOS/Tests/2026-10-13/Pull%20Test%20%C3%A9%231.csv");
    TT_CHECK(!e[1].is_collection);
    TT_EQ_INT(e[1].content_length, 3127);
    TT_EQ_STR(e[1].etag, "\"a3f5c0d1e2b7489f9c1d2e3f4a5b6c7d\"");
    TT_EQ_INT(e[1].status, 200);
    char dec[TU_HREF_MAX];
    tu_percent_decode(e[1].href, dec, sizeof dec);
    TT_EQ_STR(dec, "/remote.php/dav/files/tensile-hmi/TensileOS/Tests/2026-10-13/Pull Test \xC3\xA9#1.csv");

    TT_EQ_STR(e[2].etag, "\"0b1c2d3e4f5061728394a5b6c7d8e9f0\"");

    /* fewer slots than responses: counts all, fills what fits */
    TT_EQ_INT(tu_propfind_parse(NC_PROPFIND_207, strlen(NC_PROPFIND_207), e, 1, &n), TU_OK);
    TT_EQ_INT(n, 3);
    TT_CHECK(e[0].is_collection);
    TT_EQ_INT(tu_propfind_parse(NC_PROPFIND_207, strlen(NC_PROPFIND_207), NULL, 0, &n), TU_OK);
    TT_EQ_INT(n, 3);
}

/* Apache mod_dav style: uppercase D:, live props in a prefix declared on a nested element. */
static const char APACHE_207[] =
    "<?xml version=\"1.0\" encoding=\"utf-8\"?>\r\n"
    "<D:multistatus xmlns:D=\"DAV:\">\r\n"
    "  <D:response xmlns:lp1=\"DAV:\" xmlns:lp2=\"http://apache.org/dav/props/\">\r\n"
    "    <D:href>/dav/TensileOS/</D:href>\r\n"
    "    <D:propstat>\r\n"
    "      <D:prop>\r\n"
    "        <lp1:resourcetype>\r\n          <D:collection/>\r\n        </lp1:resourcetype>\r\n"
    "        <lp1:getetag>  \"1000-5f2c3b4a\"  </lp1:getetag>\r\n"
    "        <lp2:executable>F</lp2:executable>\r\n"
    "      </D:prop>\r\n"
    "      <D:status>HTTP/1.1 200 OK</D:status>\r\n"
    "    </D:propstat>\r\n"
    "  </D:response>\r\n"
    "</D:multistatus>\r\n";

/* Default namespace, comments, CDATA, numeric entities, response-level status, foreign "status". */
static const char DEFAULT_NS_207[] =
    "<?xml version='1.0'?>\n<!-- generated -->\n"
    "<multistatus xmlns='DAV:' xmlns:x='urn:example'>\n"
    " <response>\n"
    "  <href><![CDATA[/a/b%20c.csv]]></href>\n"
    "  <propstat>\n"
    "   <prop><getcontentlength>\n 42 \n</getcontentlength><getetag>&#34;e&#x31;&#34;</getetag>"
    "<x:status>HTTP/1.1 500 Wrong</x:status><resourcetype /></prop>\n"
    "   <status>HTTP/1.1 200 OK</status>\n"
    "  </propstat>\n"
    " </response>\n"
    " <response><href>/a/gone.csv</href><status>HTTP/1.1 404 Not Found</status></response>\n"
    "</multistatus>";

static void test_propfind_variants(void)
{
    tu_dav_entry_t e[4];
    size_t n = 0;
    TT_EQ_INT(tu_propfind_parse(APACHE_207, strlen(APACHE_207), e, 4, &n), TU_OK);
    TT_EQ_INT(n, 1);
    TT_EQ_STR(e[0].href, "/dav/TensileOS/");
    TT_CHECK(e[0].is_collection);
    TT_EQ_STR(e[0].etag, "\"1000-5f2c3b4a\"");
    TT_EQ_INT(e[0].status, 200);

    TT_EQ_INT(tu_propfind_parse(DEFAULT_NS_207, strlen(DEFAULT_NS_207), e, 4, &n), TU_OK);
    TT_EQ_INT(n, 2);
    TT_EQ_STR(e[0].href, "/a/b%20c.csv");
    TT_EQ_INT(e[0].content_length, 42);
    TT_EQ_STR(e[0].etag, "\"e1\"");
    TT_EQ_INT(e[0].status, 200); /* x:status ignored */
    TT_CHECK(!e[0].is_collection);
    TT_EQ_STR(e[1].href, "/a/gone.csv");
    TT_EQ_INT(e[1].status, 404);
    TT_CHECK(!e[1].has_etag);

    /* props only from 2xx propstats */
    static const char only404[] =
        "<d:multistatus xmlns:d=\"DAV:\"><d:response><d:href>/x</d:href><d:propstat><d:prop>"
        "<d:getetag>\"nope\"</d:getetag></d:prop><d:status>HTTP/1.1 404 Not Found</d:status>"
        "</d:propstat></d:response></d:multistatus>";
    TT_EQ_INT(tu_propfind_parse(only404, strlen(only404), e, 4, &n), TU_OK);
    TT_EQ_INT(n, 1);
    TT_CHECK(!e[0].has_etag);
    TT_EQ_INT(e[0].status, 404);

    /* not a multistatus (captive portal HTML) */
    static const char html[] = "<html><body><h1>Login</h1><a href=\"/x\">x</a></body></html>";
    TT_EQ_INT(tu_propfind_parse(html, strlen(html), e, 4, &n), TU_ERR_PARSE);
    TT_EQ_INT(n, 0);

    /* truncated body: parses complete responses, no crash */
    size_t cut = strlen(NC_PROPFIND_207) / 2;
    TT_EQ_INT(tu_propfind_parse(NC_PROPFIND_207, cut, e, 4, &n), TU_OK);
    TT_CHECK(n >= 1 && n < 3);
    for (size_t k = 0; k < strlen(NC_PROPFIND_207); k += 37) tu_propfind_parse(NC_PROPFIND_207, k, e, 4, &n);

    TT_CHECK(strstr(TU_PROPFIND_BODY, "<d:getetag/>") != NULL);
}

/* ------------------------------------------------------------------------ */
static uint32_t rng_seq(void *ctx)
{
    uint32_t *s = ctx;
    *s = *s * 1664525u + 1013904223u;
    return *s;
}
static uint32_t rng_zero(void *ctx) { (void)ctx; return 0; }
static uint32_t rng_max(void *ctx) { (void)ctx; return 0xFFFFFFFFu; }

static void test_backoff(void)
{
    TT_EQ_INT(tu_backoff_ms(0, NULL, NULL), 5000);
    TT_EQ_INT(tu_backoff_ms(1, NULL, NULL), 10000);
    TT_EQ_INT(tu_backoff_ms(2, NULL, NULL), 20000);
    TT_EQ_INT(tu_backoff_ms(7, NULL, NULL), 640000);
    TT_EQ_INT(tu_backoff_ms(8, NULL, NULL), 900000);
    TT_EQ_INT(tu_backoff_ms(1000, NULL, NULL), 900000);
    TT_EQ_INT(tu_backoff_ms(0xFFFFFFFFu, NULL, NULL), 900000);
    /* jitter bounds: -20 % / +20 %, clamped to [5 s, 15 min] */
    TT_EQ_INT(tu_backoff_ms(3, rng_zero, NULL), 32000);   /* 40 s - 20 % */
    TT_EQ_INT(tu_backoff_ms(0, rng_zero, NULL), 5000);    /* clamped up */
    TT_EQ_INT(tu_backoff_ms(20, rng_zero, NULL), 720000); /* 15 min - 20 % */
    uint32_t seed = 1;
    for (uint32_t a = 0; a < 30; a++) {
        for (int k = 0; k < 50; k++) {
            uint32_t d = tu_backoff_ms(a, rng_seq, &seed);
            TT_CHECK(d >= TU_BACKOFF_MIN_MS && d <= TU_BACKOFF_MAX_MS);
            uint32_t nominal = tu_backoff_ms(a, NULL, NULL);
            TT_CHECK(d >= nominal - nominal / 5 || d == TU_BACKOFF_MIN_MS);
            TT_CHECK(d <= nominal + nominal / 5);
        }
    }
    (void)rng_max;
    TT_CHECK(tu_backoff_ms(2, rng_max, NULL) <= 24000);

    tu_class_t c = {TU_RES_RETRY_LATER, 503, 30};
    TT_EQ_INT(tu_retry_delay_ms(&c, 0, NULL, NULL), 30000);
    uint32_t d = tu_retry_delay_ms(&c, 0, rng_seq, &seed);
    TT_CHECK(d >= 30000 && d <= 33000);
    c.retry_after_s = 0;
    TT_EQ_INT(tu_retry_delay_ms(&c, 5, NULL, NULL), 1000);
    c.retry_after_s = 86400;
    TT_EQ_INT(tu_retry_delay_ms(&c, 0, NULL, NULL), 3600000);
    c.retry_after_s = -1;
    TT_EQ_INT(tu_retry_delay_ms(&c, 2, NULL, NULL), 20000);
}

/* ------------------------------------------------------------------------ */
static void test_collision_name(void)
{
    char out[TU_PATH_MAX];
    TT_EQ_INT(tu_collision_name("Tests/2026-10-14/name.csv", NULL, 0, out, sizeof out), TU_OK);
    TT_EQ_STR(out, "Tests/2026-10-14/name.csv");
    TT_EQ_INT(tu_collision_name("Tests/2026-10-14/name.csv", NULL, 1, out, sizeof out), TU_OK);
    TT_EQ_STR(out, "Tests/2026-10-14/name_TAB5.csv");
    TT_EQ_INT(tu_collision_name("Tests/2026-10-14/name.csv", TU_COLLISION_SUFFIX, 2, out, sizeof out), TU_OK);
    TT_EQ_STR(out, "Tests/2026-10-14/name_TAB5_2.csv");
    TT_EQ_INT(tu_collision_name("Tests/2026-10-14/name.csv", NULL, 13, out, sizeof out), TU_OK);
    TT_EQ_STR(out, "Tests/2026-10-14/name_TAB5_13.csv");
    TT_EQ_INT(tu_collision_name("a.b/name.tar.gz", NULL, 1, out, sizeof out), TU_OK);
    TT_EQ_STR(out, "a.b/name.tar_TAB5.gz");
    TT_EQ_INT(tu_collision_name("dir.v2/README", NULL, 1, out, sizeof out), TU_OK);
    TT_EQ_STR(out, "dir.v2/README_TAB5");
    TT_EQ_INT(tu_collision_name(".hidden", NULL, 1, out, sizeof out), TU_OK);
    TT_EQ_STR(out, ".hidden_TAB5");
    TT_EQ_INT(tu_collision_name("x/S1.csv", TU_EDIT_SUFFIX, 1, out, sizeof out), TU_OK);
    TT_EQ_STR(out, "x/S1_hmi-edit.csv");
    TT_EQ_INT(tu_collision_name("x/", NULL, 1, out, sizeof out), TU_ERR_ARG);
    char small[8];
    TT_EQ_INT(tu_collision_name("name.csv", NULL, 1, small, sizeof small), TU_ERR_OVERFLOW);

    TT_EQ_STR(tu_content_type_for("a/B.CSV"), "text/csv");
    TT_EQ_STR(tu_content_type_for("Stats_120000.pdf"), "application/pdf");
    TT_EQ_STR(tu_content_type_for("x.bin"), "application/octet-stream");
}

/* ------------------------------------------------------------------------ */
/* In-memory fake WebDAV server used to drive the planner. */
#define FS_MAX 32
typedef struct {
    char paths[FS_MAX][TU_PATH_MAX]; /* collections end with '/' */
    int n;
    int auth_fail_after; /* -1 = never; else return 401 once request count reaches this */
    int requests;
    int fail_status;     /* if nonzero, return this for every request */
    char log[64][TU_PATH_MAX + 8];
} fakefs_t;

static bool fs_has(const fakefs_t *fs, const char *p)
{
    for (int i = 0; i < fs->n; i++)
        if (strcmp(fs->paths[i], p) == 0) return true;
    return false;
}
static void fs_add(fakefs_t *fs, const char *p)
{
    if (fs->n < FS_MAX) snprintf(fs->paths[fs->n++], TU_PATH_MAX, "%s", p);
}
static bool fs_parent_exists(const fakefs_t *fs, const char *p)
{
    char par[TU_PATH_MAX];
    snprintf(par, sizeof par, "%s", p);
    size_t n = strlen(par);
    if (n && par[n - 1] == '/') par[--n] = '\0';
    char *s = strrchr(par, '/');
    if (!s) return true; /* parent is the base collection */
    s[1] = '\0';
    return fs_has(fs, par);
}

static int fs_handle(fakefs_t *fs, const tu_step_t *st)
{
    int idx = fs->requests++;
    if (idx < 64) snprintf(fs->log[idx], sizeof fs->log[idx], "%s %s", st->kind == TU_STEP_MKCOL ? "MKCOL" : "PUT", st->path);
    if (fs->auth_fail_after >= 0 && idx >= fs->auth_fail_after) return 401;
    if (fs->fail_status) return fs->fail_status;
    if (st->kind == TU_STEP_MKCOL) {
        char asfile[TU_PATH_MAX];
        snprintf(asfile, sizeof asfile, "%s", st->path);
        asfile[strlen(asfile) - 1] = '\0';
        if (fs_has(fs, st->path) || fs_has(fs, asfile)) return 405;
        if (!fs_parent_exists(fs, st->path)) return 409;
        fs_add(fs, st->path);
        return 201;
    }
    /* PUT */
    if (!fs_parent_exists(fs, st->path)) return 409;
    bool exists = fs_has(fs, st->path);
    if (st->precond == TU_PRECOND_IF_NONE_MATCH_STAR && exists) return 412;
    if (st->precond == TU_PRECOND_IF_MATCH && (!exists || strcmp(st->etag, "\"cur\"") != 0)) return 412;
    if (exists) return 204;
    fs_add(fs, st->path);
    return 201;
}

static tu_step_kind_t run_plan(tu_plan_t *p, fakefs_t *fs)
{
    for (int guard = 0; guard < 100; guard++) {
        tu_step_t st = tu_plan_step(p);
        if (st.kind == TU_STEP_DONE || st.kind == TU_STEP_FAILED) return st.kind;
        int status = fs_handle(fs, &st);
        tu_class_t c = tu_classify(st.kind == TU_STEP_MKCOL ? TU_METHOD_MKCOL : TU_METHOD_PUT, status, NULL, 0);
        TT_EQ_INT(tu_plan_feed(p, &c), TU_OK);
    }
    return TU_STEP_FAILED;
}

static void fs_init(fakefs_t *fs)
{
    memset(fs, 0, sizeof *fs);
    fs->auth_fail_after = -1;
}

static void test_planner(void)
{
    fakefs_t fs;
    tu_plan_t p;

    /* 1. fresh account: Tests/ missing -> MKCOL date (409), MKCOL Tests (201), MKCOL date (201), PUT (201) */
    fs_init(&fs);
    TT_EQ_INT(tu_plan_init(&p, "Tests/2026-10-14/S1.csv", NULL), TU_OK);
    TT_EQ_INT(p.n_dirs, 2);
    TT_EQ_INT(run_plan(&p, &fs), TU_STEP_DONE);
    TT_EQ_INT(fs.requests, 4);
    TT_EQ_STR(fs.log[0], "MKCOL Tests/2026-10-14/");
    TT_EQ_STR(fs.log[1], "MKCOL Tests/");
    TT_EQ_STR(fs.log[2], "MKCOL Tests/2026-10-14/");
    TT_EQ_STR(fs.log[3], "PUT Tests/2026-10-14/S1.csv");
    TT_CHECK(p.created);
    TT_EQ_STR(tu_plan_step(&p).path, "Tests/2026-10-14/S1.csv");

    /* 2. same day, second file: MKCOL 405 then PUT 201 */
    fs.requests = 0;
    TT_EQ_INT(tu_plan_init(&p, "Tests/2026-10-14/S2.csv", NULL), TU_OK);
    TT_EQ_INT(run_plan(&p, &fs), TU_STEP_DONE);
    TT_EQ_INT(fs.requests, 2);
    TT_EQ_INT(p.last.http_status, 201);

    /* 3. collision: S1.csv exists -> S1_TAB5.csv; again -> S1_TAB5_2.csv */
    fs.requests = 0;
    TT_EQ_INT(tu_plan_init(&p, "Tests/2026-10-14/S1.csv", NULL), TU_OK);
    TT_EQ_INT(run_plan(&p, &fs), TU_STEP_DONE);
    TT_EQ_STR(p.path, "Tests/2026-10-14/S1_TAB5.csv");
    TT_EQ_INT(fs.requests, 3);
    fs.requests = 0;
    TT_EQ_INT(tu_plan_init(&p, "Tests/2026-10-14/S1.csv", NULL), TU_OK);
    TT_EQ_INT(run_plan(&p, &fs), TU_STEP_DONE);
    TT_EQ_STR(p.path, "Tests/2026-10-14/S1_TAB5_2.csv");
    TT_EQ_STR(fs.log[3], "PUT Tests/2026-10-14/S1_TAB5_2.csv");

    /* 4. collision limit */
    tu_plan_opts_t o = {TU_PRECOND_IF_NONE_MATCH_STAR, NULL, true, 1};
    fs.requests = 0;
    TT_EQ_INT(tu_plan_init(&p, "Tests/2026-10-14/S1.csv", &o), TU_OK);
    TT_EQ_INT(run_plan(&p, &fs), TU_STEP_FAILED);
    TT_EQ_INT(p.fail.result, TU_RES_PRECONDITION_FAILED);
    TT_EQ_INT(fs.requests, 2);

    /* 5. dirs_known: PUT straight away; wrong guess -> 409 -> MKCOL -> PUT */
    o = (tu_plan_opts_t){TU_PRECOND_IF_NONE_MATCH_STAR, NULL, true, 0};
    fs.requests = 0;
    TT_EQ_INT(tu_plan_init(&p, "Tests/2026-10-14/S3.csv", &o), TU_OK);
    TT_EQ_INT(run_plan(&p, &fs), TU_STEP_DONE);
    TT_EQ_INT(fs.requests, 1);
    fs.requests = 0;
    TT_EQ_INT(tu_plan_init(&p, "Tests/2026-10-15/S1.csv", &o), TU_OK);
    TT_EQ_INT(run_plan(&p, &fs), TU_STEP_DONE);
    TT_EQ_INT(fs.requests, 3);
    TT_EQ_STR(fs.log[0], "PUT Tests/2026-10-15/S1.csv");
    TT_EQ_STR(fs.log[1], "MKCOL Tests/2026-10-15/");
    TT_EQ_STR(fs.log[2], "PUT Tests/2026-10-15/S1.csv");

    /* 6. If-Match edit: matching etag -> 204 replace; stale -> _hmi-edit copy */
    o = (tu_plan_opts_t){TU_PRECOND_IF_MATCH, "\"cur\"", true, 0};
    fs.requests = 0;
    TT_EQ_INT(tu_plan_init(&p, "Tests/2026-10-14/S2.csv", &o), TU_OK);
    TT_EQ_INT(tu_plan_step(&p).precond, TU_PRECOND_IF_MATCH);
    TT_EQ_STR(tu_plan_step(&p).etag, "\"cur\"");
    TT_EQ_INT(run_plan(&p, &fs), TU_STEP_DONE);
    TT_CHECK(!p.created);
    TT_EQ_STR(p.path, "Tests/2026-10-14/S2.csv");
    o.etag = "\"stale\"";
    fs.requests = 0;
    TT_EQ_INT(tu_plan_init(&p, "Tests/2026-10-14/S2.csv", &o), TU_OK);
    TT_EQ_INT(run_plan(&p, &fs), TU_STEP_DONE);
    TT_EQ_STR(p.path, "Tests/2026-10-14/S2_hmi-edit.csv");
    TT_CHECK(p.created);
    fs.requests = 0;
    TT_EQ_INT(tu_plan_init(&p, "Tests/2026-10-14/S2.csv", &o), TU_OK);
    TT_EQ_INT(run_plan(&p, &fs), TU_STEP_DONE);
    TT_EQ_STR(p.path, "Tests/2026-10-14/S2_hmi-edit_2.csv");
    o.etag = NULL;
    TT_EQ_INT(tu_plan_init(&p, "Tests/x.csv", &o), TU_ERR_ARG);

    /* 7. 401 stops immediately, no further requests */
    fs.requests = 0;
    fs.auth_fail_after = 0;
    TT_EQ_INT(tu_plan_init(&p, "Tests/2026-10-16/S1.csv", NULL), TU_OK);
    TT_EQ_INT(run_plan(&p, &fs), TU_STEP_FAILED);
    TT_EQ_INT(p.fail.result, TU_RES_AUTH_FAILED);
    TT_EQ_INT(fs.requests, 1);
    fs.requests = 0;
    fs.auth_fail_after = 1; /* MKCOL ok, PUT 401 */
    TT_EQ_INT(tu_plan_init(&p, "Tests/2026-10-14/S9.csv", NULL), TU_OK);
    TT_EQ_INT(run_plan(&p, &fs), TU_STEP_FAILED);
    TT_EQ_INT(p.fail.result, TU_RES_AUTH_FAILED);
    TT_EQ_INT(fs.requests, 2);
    tu_class_t again = {TU_RES_CREATED, 201, -1};
    TT_EQ_INT(tu_plan_feed(&p, &again), TU_ERR_STATE);
    fs.auth_fail_after = -1;

    /* 8. 503 -> FAILED/RETRY_LATER with Retry-After carried through */
    TT_EQ_INT(tu_plan_init(&p, "Tests/2026-10-14/S9.csv", NULL), TU_OK);
    tu_class_t busy = tu_classify(TU_METHOD_MKCOL, 503, "120", 0);
    TT_EQ_INT(tu_plan_feed(&p, &busy), TU_OK);
    TT_EQ_INT(tu_plan_step(&p).kind, TU_STEP_FAILED);
    TT_EQ_INT(p.fail.result, TU_RES_RETRY_LATER);
    TT_EQ_INT(p.fail.retry_after_s, 120);

    /* 9. a FILE named like the folder blocks MKCOL (405) then PUT 409: fail, no loop */
    fs_init(&fs);
    fs_add(&fs, "Tests/");
    fs_add(&fs, "Tests/2026-10-20"); /* file, not collection */
    TT_EQ_INT(tu_plan_init(&p, "Tests/2026-10-20/S1.csv", NULL), TU_OK);
    TT_EQ_INT(run_plan(&p, &fs), TU_STEP_FAILED);
    TT_EQ_INT(p.fail.result, TU_RES_NOT_FOUND);
    TT_CHECK(fs.requests <= 3);
    /* ... and a file "Tests" with deeper path: MKCOL x/y (409) -> MKCOL x (405) -> fail, no up/down loop */
    fs_init(&fs);
    fs_add(&fs, "A");
    TT_EQ_INT(tu_plan_init(&p, "A/B/c.csv", NULL), TU_OK);
    TT_EQ_INT(run_plan(&p, &fs), TU_STEP_FAILED);
    TT_EQ_INT(p.fail.result, TU_RES_NOT_FOUND);
    TT_EQ_INT(fs.requests, 3);

    /* 10. file at base root: PUT only */
    fs_init(&fs);
    TT_EQ_INT(tu_plan_init(&p, "/hello.csv", NULL), TU_OK);
    TT_EQ_INT(p.n_dirs, 0);
    TT_EQ_INT(run_plan(&p, &fs), TU_STEP_DONE);
    TT_EQ_INT(fs.requests, 1);

    /* 11. deep path walks all the way up */
    fs_init(&fs);
    TT_EQ_INT(tu_plan_init(&p, "Reports/2026-10-14/a/b/Stats_120000.pdf", NULL), TU_OK);
    TT_EQ_INT(run_plan(&p, &fs), TU_STEP_DONE);
    TT_EQ_INT(fs.requests, 8); /* 3 x 409 going up, 201 at Reports/, 3 x 201 down, PUT */

    /* 12. bad inputs */
    TT_EQ_INT(tu_plan_init(&p, "Tests/../x.csv", NULL), TU_ERR_PATH);
    TT_EQ_INT(tu_plan_init(&p, "Tests//x.csv", NULL), TU_ERR_PATH);
    TT_EQ_INT(tu_plan_init(&p, "Tests/", NULL), TU_ERR_PATH);
    TT_EQ_INT(tu_plan_init(&p, "", NULL), TU_ERR_ARG);
    TT_EQ_INT(tu_plan_init(&p, "1/2/3/4/5/6/7/8/9/x.csv", NULL), TU_ERR_PATH);
    TT_EQ_INT(tu_plan_step(&p).kind, TU_STEP_FAILED);
}

/* ------------------------------------------------------------------------ */
static void test_conn_message(void)
{
    char m[128];
    TT_CHECK(tu_conn_test_message(BASE, TU_NET_OK, 207, true, m, sizeof m));
    TT_EQ_STR(m, "Connected");
    TT_CHECK(!tu_conn_test_message(BASE, TU_NET_OK, 401, true, m, sizeof m));
    TT_EQ_STR(m, "App password rejected");
    TT_CHECK(!tu_conn_test_message(BASE, TU_NET_OK, 404, true, m, sizeof m));
    TT_EQ_STR(m, "Folder TensileOS not found under tensile-hmi");
    TT_CHECK(!tu_conn_test_message("https://h/remote.php/dav/files/john%20doe/My%20Data/", TU_NET_OK, 404, true, m,
                                   sizeof m));
    TT_EQ_STR(m, "Folder My Data not found under john doe");
    TT_CHECK(!tu_conn_test_message(BASE, TU_NET_OK, 207, false, m, sizeof m));
    TT_EQ_STR(m, "TensileOS is a file, not a folder");
    TT_CHECK(!tu_conn_test_message(BASE, TU_NET_OK, 200, true, m, sizeof m));
    TT_CHECK(strstr(m, "captive portal") != NULL);
    TT_CHECK(!tu_conn_test_message(BASE, TU_NET_TLS, 0, true, m, sizeof m));
    TT_CHECK(strstr(m, "TLS") != NULL);
    TT_CHECK(!tu_conn_test_message(BASE, TU_NET_DNS, 0, true, m, sizeof m));
    TT_CHECK(!tu_conn_test_message(BASE, TU_NET_OK, 503, true, m, sizeof m));
    TT_CHECK(strstr(m, "503") != NULL);
}

int main(void)
{
    TT_RUN(test_url_builder);
    TT_RUN(test_auth);
    TT_RUN(test_http_date);
    TT_RUN(test_retry_after);
    TT_RUN(test_classify);
    TT_RUN(test_propfind_nextcloud);
    TT_RUN(test_propfind_variants);
    TT_RUN(test_backoff);
    TT_RUN(test_collision_name);
    TT_RUN(test_planner);
    TT_RUN(test_conn_message);
    return tt_summary();
}
