# 01: Downloads follow redirects

**What to build:** A package, its signature, a firmware, an artifact manifest
and its signature download from a URL that answers with a redirect, as every
GitHub Release asset URL does (a 302 to `release-assets.githubusercontent.com`).
Today the catalog fetch follows redirects and everything else refuses any
answer that is not a 2xx. A download follows at most five redirects and never
goes from HTTPS to HTTP; one that starts on plain HTTP, as the simulator's
fakes do, may redirect within HTTP. What is installed is still trusted for its
signature and hash alone, never for the host it came from. Spec: `../spec.md`,
"Downloads".

**Blocked by:** None (can start immediately)

**Status:** done

- [x] An App Store package whose URL redirects to another host downloads,
      with its signature from a redirecting URL too, and reaches the
      signature check (the simulator trusts only the kernel's key, so an
      install there stops at the signature, as now).
- [x] A sixth redirect is refused, and so is a redirect from HTTPS to HTTP,
      each with a log line saying why.
- [x] The rule (count, scheme) is decided in one place, tested on the host.
- [x] A simulator test runs the App Store against a fake host that answers
      asset URLs with 302s to a second fake, in the manner of the existing
      App Store tests.
- [x] The Podcasts app's own redirect handling is left as it is.

## Comments

2026-10-26, implemented on branch `worktree-agent-a4689042c8ba1f06f`:

- The rule is `components/kernel_rs/src/redirect_policy.rs`: `is_redirect()`
  (301, 302, 303, 307, 308) and `follow(from, to, followed)`: at most 5,
  never HTTPS to HTTP, only to `http://` or `https://`. Seven host tests.
- `http_client_shim.c` has `thistle_http_client_init_download()`, a client
  with ESP-IDF's `disable_auto_redirect` set, and
  `thistle_http_client_redirect()` (`esp_http_client_set_redirection()`,
  then `esp_http_client_get_url()` for the policy and the log). The App
  Store client's catalog fetch (`perform()`) and every streamed download
  (`download_into_staged()`: package, signature, firmware) follow 3xx
  answers through the policy. A refusal logs `Download refused: <why>, to
  <url>` and fails with ESP_FAIL, so the toast says "download failed". The
  catalog no longer follows through ESP-IDF's own redirect handling, which
  allowed 10 and any scheme.
- GPS assistance (`gps_assist.rs`) still uses `thistle_http_client_init()`,
  which follows on its own, as before; it is not one of the spec's
  downloads.
- The simulator's HTTP shim now keeps a redirect's Location for `perform()`
  too and has `esp_http_client_get_url()`. In the simulator the thistle
  shim no longer hands a redirect's body to the caller, matching the device.
- `simulator/tests/test_appstore.py`: a catalog, and a package with its
  signature, behind 302s to a second fake host; five relative redirects
  followed; a sixth refused with its log line. HTTPS to HTTP is covered by
  the host tests only, as the fakes are plain HTTP.
- Podcasts' `pod_net.c` is untouched.

Left for the hardware: on the T-Deck Max, an install whose URLs are a real
GitHub Release asset's (a 302 to `release-assets.githubusercontent.com`,
with a query string) downloads and reaches the signature check, and a
refresh of a catalog behind a redirect works over HTTPS. The redirect's URL
in the log is ESP-IDF's `get_url()`, which leaves out the query.
