images = jpeg420exif.jpg jpeg422jfif.jpg jpeg400jfif.jpg jpeg444.jpg

$(images):
	wget "https://www.w3.org/MarkUp/Test/xhtml-print/20050519/tests/$@" -q

test: jpeg.c test.c $(images)
	cc $^ -o test -lm
	./test jpeg420exif.jpg
	./test jpeg422jfif.jpg
	./test jpeg400jfif.jpg
	./test jpeg444.jpg

python:
	python jpeg_python/setup.py build_ext -i

format:
	clang-format -i jpeg.c jpeg.h test.c jpeg_python/jpeg_python.c
