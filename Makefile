test:
	cc jpeg.c test.c -o test -lm
	./test sample.jpg

python:
	python jpeg_python/setup.py build_ext -i

format:
	clang-format -i jpeg.c jpeg.h test.c jpeg_python/jpeg_python.c
