# https://setuptools.pypa.io/en/latest/userguide/ext_modules.html
from setuptools import Extension, setup
from pathlib import Path

CURRENT_DIR = Path(__file__).parent
PARENT_DIR = CURRENT_DIR.parent

setup(
    name="jpeg_python",
    version="1.0.0",
    description="Python interface for the fputs C library function",
    author="<your name>",
    author_email="your_email@gmail.com",
    ext_modules=[
        Extension(
            name="jpeg_python",
            sources=[str(PARENT_DIR / "jpeg.c"), str(CURRENT_DIR / "jpeg_python.c")],
            include_dirs=[str(PARENT_DIR)],
        )
    ],
)
