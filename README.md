# jpeg.c

C port of https://github.com/gau-nernst/jpeg-python

Only JPEG Baseline is implemented.

Some test images: https://www.w3.org/MarkUp/Test/xhtml-print/20050519/tests/A_2_1-BF-01.htm

Linux and MacOS

```bash
make test
```

Windows

```bash
cl /c jpeg.c && lib jpeg.obj
cl test.c jpeg.lib && ./test sample.jpg
```
