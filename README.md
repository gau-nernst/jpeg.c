# jpeg.c

C port of https://github.com/gau-nernst/jpeg-python

Only JPEG Baseline is implemented. Basic support for restart markers.

Some test images: https://www.w3.org/MarkUp/Test/xhtml-print/20050519/tests/A_2_1-BF-01.htm
- I cannot get https://www.w3.org/MarkUp/Test/xhtml-print/20050519/tests/jpeg444.jpg decoded correctly. Not sure why.

Linux and MacOS

```bash
make test_all
```

Windows

```bash
cl test.c jpeg.c && ./test sample.jpg
```
