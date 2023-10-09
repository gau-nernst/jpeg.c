static_lib:
	clang-format -i jpeg.c
	cc -c -fPIC jpeg.c -o jpeg.o
	ar rcs libjpeg_foo.a jpeg.o

test: static_lib
	clang-format -i test.c
	cc test.c -o test -ljpeg_foo -lm -L.
	./test sample.jpg
