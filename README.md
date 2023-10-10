# jpeg.c

C port of https://github.com/gau-nernst/jpeg-python

Only JPEG Baseline is implemented.

Linux and MacOS

```bash
make test
```

Windows

```bash
cl /c jpeg.c && lib jpeg.obj
cl test.c jpeg.lib && ./test sample.jpg
```
