/*
 * SPDX-License-Identifier: BSD-3-Clause
 * ThistleOS — File Transfer: the web page.
 *
 * web.cmake writes web/index.html into xfer_web_index.h in the build
 * directory, as the bytes of XFER_INDEX_HTML. A build that does not (the
 * browser simulator, which has no server to serve it) gets a line instead.
 */
#include "filexfer/xfer_web.h"

#if defined(__has_include)
#if __has_include("xfer_web_index.h")
#include "xfer_web_index.h"
#endif
#endif

#ifdef XFER_INDEX_HTML
const char xfer_index_html[] = {XFER_INDEX_HTML};
#else
const char xfer_index_html[] = "<!doctype html><title>ThistleOS</title><p>This build has no web page.";
#endif

const size_t xfer_index_html_len = sizeof xfer_index_html - 1;
