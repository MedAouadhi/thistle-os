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

**Status:** ready-for-agent

- [ ] An App Store package whose URL redirects to another host downloads,
      with its signature from a redirecting URL too, and reaches the
      signature check (the simulator trusts only the kernel's key, so an
      install there stops at the signature, as now).
- [ ] A sixth redirect is refused, and so is a redirect from HTTPS to HTTP,
      each with a log line saying why.
- [ ] The rule (count, scheme) is decided in one place, tested on the host.
- [ ] A simulator test runs the App Store against a fake host that answers
      asset URLs with 302s to a second fake, in the manner of the existing
      App Store tests.
- [ ] The Podcasts app's own redirect handling is left as it is.
