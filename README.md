# jpeg.c

C port of https://github.com/gau-nernst/jpeg-python

Only JPEG Baseline is implemented.

Linux

```bash
gcc jpeg.c -o jpeg -lm
./jpeg sample.jpg
```

MacOS

```bash
clang jpeg.c -o jpeg
./jpeg sample.jpg
```

Windows

```bash
cl jpeg.c
./jpeg sample.jpg
```
