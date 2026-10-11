#!/usr/bin/env python3
# SPDX-License-Identifier: BSD-3-Clause
"""Write the EPUBs test_epub.c reads into the directory given.

    make_fixtures.py OUT_DIR

Each is built here rather than checked in, so the bytes under test are the
ones this file shows: an EPUB 2 book with an NCX, an EPUB 3 Arabic book with a
navigation document, HTML that is not XML, and archives broken in the ways a
card's files can be.
"""

import os
import sys
import zipfile

CONTAINER = """<?xml version="1.0"?>
<container version="1.0" xmlns="urn:oasis:names:tc:opendocument:xmlns:container">
  <rootfiles>
    <rootfile full-path="{opf}" media-type="application/oebps-package+xml"/>
  </rootfiles>
</container>
"""

EPUB2_OPF = """<?xml version="1.0" encoding="UTF-8"?>
<package xmlns="http://www.idpf.org/2007/opf" version="2.0" unique-identifier="id">
  <metadata xmlns:dc="http://purl.org/dc/elements/1.1/">
    <dc:title>  A  Test
      Book </dc:title>
    <dc:title>A Subtitle</dc:title>
    <dc:language>en</dc:language>
  </metadata>
  <manifest>
    <item id="ncx" href="toc.ncx" media-type="application/x-dtbncx+xml"/>
    <item id="cover" href="cover.xhtml" media-type="application/xhtml+xml"/>
    <item id="ch1" href="Text/ch1.xhtml" media-type="application/xhtml+xml"/>
    <item id="pic" href="Images/pic.jpg" media-type="image/jpeg"/>
    <item id="ch2" href="Text/Chapter%202.xhtml" media-type="application/xhtml+xml"/>
  </manifest>
  <spine toc="ncx">
    <itemref idref="cover"/>
    <itemref idref="ch1"/>
    <itemref idref="pic"/>
    <itemref idref="ch2"/>
    <itemref idref="not-in-manifest"/>
  </spine>
</package>
"""

EPUB2_NCX = """<?xml version="1.0" encoding="UTF-8"?>
<ncx xmlns="http://www.daisy.org/z3986/2005/ncx/" version="2005-1">
  <navMap>
    <navPoint id="n1"><navLabel><text>Cover</text></navLabel><content src="cover.xhtml"/></navPoint>
    <navPoint id="n2"><navLabel><text>Chapter
      One</text></navLabel><content src="Text/ch1.xhtml#start"/>
      <navPoint id="n3"><navLabel><text>A Section</text></navLabel><content src="Text/ch1.xhtml#s2"/></navPoint>
    </navPoint>
    <navPoint id="n4"><navLabel><text>Chapter Two</text></navLabel><content src="Text/Chapter%202.xhtml"/></navPoint>
    <navPoint id="n5"><navLabel><text>Missing</text></navLabel><content src="Text/nope.xhtml"/></navPoint>
  </navMap>
</ncx>
"""

XHTML11 = ('<?xml version="1.0" encoding="UTF-8"?>\n'
           '<!DOCTYPE html PUBLIC "-//W3C//DTD XHTML 1.1//EN" '
           '"http://www.w3.org/TR/xhtml11/DTD/xhtml11.dtd">\n')

COVER = XHTML11 + """<html xmlns="http://www.w3.org/1999/xhtml"><head><title>Cover</title></head>
<body><div><img src="Images/pic.jpg" alt="cover"/></div></body></html>
"""

CH1 = XHTML11 + """<html xmlns="http://www.w3.org/1999/xhtml">
<head><title>Not text</title><style>p { margin: 0 }</style></head>
<body>
  <h1 id="start">Chapter   One</h1>
  <p>It was a <i>dark</i>&nbsp;and stormy night&mdash;or so they said &amp; wrote.</p>
  <p>&#8220;Quotes&#x201D; and it&#8217;s fine.</p>
  <p>&nbsp;</p>
  <p>Line one<br/>line two</p>
  <script>var p = "<p>not text</p>";</script>
  <ul><li><p>First</p></li><li>Second</li></ul>
  <table><tr><td>a</td><td>b</td></tr></table>
  <p id="s2">soft­hy​phen</p>
</body></html>
"""

CH2 = XHTML11 + """<html xmlns="http://www.w3.org/1999/xhtml"><head><title>Two</title></head>
<body><p>The second chapter.</p></body></html>
"""

EPUB3_OPF = """<?xml version="1.0" encoding="UTF-8"?>
<opf:package xmlns:opf="http://www.idpf.org/2007/opf" version="3.0" unique-identifier="id">
  <opf:metadata xmlns:dc="http://purl.org/dc/elements/1.1/">
    <dc:title>كتاب الاختبار</dc:title>
    <dc:language>ar</dc:language>
  </opf:metadata>
  <opf:manifest>
    <opf:item id="nav" href="nav/toc.xhtml" media-type="application/xhtml+xml" properties="nav"/>
    <opf:item id="c1" href="text/ch1.xhtml" media-type="application/xhtml+xml"/>
    <opf:item id="c2" href="text/../text/ch2.xhtml" media-type="application/xhtml+xml"/>
  </opf:manifest>
  <opf:spine>
    <opf:itemref idref="c1"/>
    <opf:itemref idref="c2"/>
  </opf:spine>
</opf:package>
"""

EPUB3_NAV = """<?xml version="1.0" encoding="UTF-8"?>
<!DOCTYPE html>
<html xmlns="http://www.w3.org/1999/xhtml" xmlns:epub="http://www.idpf.org/2007/ops">
<body>
  <nav epub:type="landmarks"><ol><li><a href="../text/ch2.xhtml">Not the toc</a></li></ol></nav>
  <nav epub:type="toc" id="toc"><ol>
    <li><a href="../text/ch1.xhtml"><span>الفصل</span> الأول</a></li>
    <li><a href="../text/ch2.xhtml#x">الفصل الثاني</a></li>
  </ol></nav>
</body></html>
"""

# An EPUB 3 document with no DTD, so &nbsp; is not XML: Expat stops, and the
# fallback has to read it.
EPUB3_CH1 = """<?xml version="1.0" encoding="UTF-8"?>
<!DOCTYPE html>
<html xmlns="http://www.w3.org/1999/xhtml"><head><title>x</title></head>
<body><h1>الفصل الأول</h1><p>مرحبا&nbsp;بالعالم.</p><p>سطر ثان.</p></body></html>
"""

EPUB3_CH2 = """<?xml version="1.0" encoding="UTF-8"?>
<html xmlns="http://www.w3.org/1999/xhtml"><body><p>نهاية</p></body></html>
"""

BROKEN_OPF = EPUB2_OPF.replace('<item id="ncx" href="toc.ncx" media-type="application/x-dtbncx+xml"/>', "")

BROKEN_CH1 = """<html><head><title>x</title>
<script>document.write("<p>not text</p>")</script></head>
<body><P>Unclosed paragraph<br>next line &amp; more &apos;quoted&apos; &#233;
<p>Second &bogus; paragraph
<!-- a comment <p>not text</p> -->
<STYLE>p { color: red }</STYLE>
<div>Last</div>
</body></html>
"""


def write_epub(path, files, compression=zipfile.ZIP_DEFLATED, comment=b""):
    with zipfile.ZipFile(path, "w") as z:
        z.writestr(zipfile.ZipInfo("mimetype", (2020, 1, 1, 0, 0, 0)),
                   "application/epub+zip", compress_type=zipfile.ZIP_STORED)
        for name, data in files:
            info = zipfile.ZipInfo(name, (2020, 1, 1, 0, 0, 0))
            z.writestr(info, data, compress_type=compression)
        z.comment = comment


def epub2_files():
    return [
        ("META-INF/container.xml", CONTAINER.format(opf="OEBPS/content.opf")),
        ("OEBPS/content.opf", EPUB2_OPF),
        ("OEBPS/toc.ncx", EPUB2_NCX),
        ("OEBPS/cover.xhtml", COVER),
        ("OEBPS/Text/ch1.xhtml", CH1),
        ("OEBPS/Images/pic.jpg", b"\xff\xd8\xff not really a jpeg"),
        ("OEBPS/Text/Chapter 2.xhtml", CH2),
    ]


def main():
    out = sys.argv[1]
    os.makedirs(out, exist_ok=True)

    write_epub(os.path.join(out, "epub2.epub"), epub2_files())

    # Stored rather than deflated, and ending in a comment the end record
    # has to be found behind.
    write_epub(os.path.join(out, "stored.epub"), epub2_files(),
               compression=zipfile.ZIP_STORED, comment=b"x" * 3000)

    write_epub(os.path.join(out, "epub3_ar.epub"), [
        ("META-INF/container.xml", CONTAINER.format(opf="package.opf")),
        ("package.opf", EPUB3_OPF),
        ("nav/toc.xhtml", EPUB3_NAV),
        ("text/ch1.xhtml", EPUB3_CH1),
        ("text/ch2.xhtml", EPUB3_CH2),
    ])

    write_epub(os.path.join(out, "broken_html.epub"), [
        ("META-INF/container.xml", CONTAINER.format(opf="OEBPS/content.opf")),
        ("OEBPS/content.opf", BROKEN_OPF),
        ("OEBPS/cover.xhtml", COVER),
        ("OEBPS/Text/ch1.xhtml", BROKEN_CH1),
        ("OEBPS/Text/Chapter 2.xhtml", CH2),
    ])

    with open(os.path.join(out, "not_zip.epub"), "wb") as f:
        f.write(b"PK\x03\x04 but nothing after it " * 40)

    with open(os.path.join(out, "epub2.epub"), "rb") as f:
        whole = f.read()
    with open(os.path.join(out, "truncated.epub"), "wb") as f:
        f.write(whole[: len(whole) // 2])

    # Chapter one's deflate stream scrambled, its sizes and headers intact.
    with zipfile.ZipFile(os.path.join(out, "epub2.epub")) as z:
        info = z.getinfo("OEBPS/Text/ch1.xhtml")
    data_start = info.header_offset + 30 + len(info.filename.encode()) + len(info.extra)
    corrupt = bytearray(whole)
    for i in range(data_start + 4, data_start + info.compress_size - 4):
        corrupt[i] ^= 0x5A
    with open(os.path.join(out, "corrupt_deflate.epub"), "wb") as f:
        f.write(corrupt)


if __name__ == "__main__":
    main()
