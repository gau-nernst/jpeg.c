static_lib:
	cc -c jpeg.c -o jpeg.o
	ar rcs libjpeg_foo.a jpeg.o

python:
	python jpeg_python/setup.py build_ext -i

test: static_lib
	cc test.c -o test -ljpeg_foo -lm -L.
	./test sample.jpg

format:
	clang-format -i jpeg.c jpeg.h test.c jpeg_python/jpeg_python.c
