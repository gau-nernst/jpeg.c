// https://docs.python.org/3/extending/extending.html
#include "jpeg.h"

#define PY_SSIZE_T_CLEAN
#include <Python.h>

static PyObject *jpeg_python_decode_jpeg(PyObject *self, PyObject *args) {
  char *filename = NULL;

  if (!PyArg_ParseTuple(args, "s", &filename))
    return NULL;

  FILE *f = fopen(filename, "rb");
  if (f == NULL) {
    PyErr_SetString(PyExc_FileNotFoundError, filename);
    return NULL;
  }
  Image8 image;

  if (decode_jpeg(f, &image)) {
    PyErr_SetString(PyExc_RuntimeError, "Error while decode JPEG");
    return NULL;
  }

  // TODO: set refcount somehow for image_buffer
  int image_size = image.width * image.height * image.n_channels;
  PyObject *out = PyMemoryView_FromMemory((char *)image.data, image_size, PyBUF_READ);
  return out;
}

// method table
static PyMethodDef JpegPythonMethods[] = {
    {"decode_jpeg", jpeg_python_decode_jpeg, METH_VARARGS, "decode_jpeg docstring"}, {NULL, NULL, 0, NULL}, // sentinel
};

// module definition
static struct PyModuleDef jpeg_module = {
    PyModuleDef_HEAD_INIT, "jpeg_python", "module docstring", -1, JpegPythonMethods,
};

// module initialization
PyMODINIT_FUNC PyInit_jpeg_python(void) { return PyModule_Create(&jpeg_module); }
