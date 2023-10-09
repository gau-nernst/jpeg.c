static_lib:
	cc -c -fPIC jpeg.c -o jpeg.o
	ar rcs libjpeg_foo.a jpeg.o

python:
	python jpeg_python/setup.py build_ext -i

test: static_lib
	cc test.c -o test -ljpeg_foo -lm -L.
	./test sample.jpg

format:
	clang-format -i jpeg.c test.c
