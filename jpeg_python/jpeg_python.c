// https://docs.python.org/3/extending/extending.html
#include "jpeg.h"
#include <stdint.h>

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

  int width, height, n_channels;
  uint8_t *image = decode_jpeg(f, &width, &height, &n_channels);

  if (image == 0) {
    PyErr_SetString(PyExc_RuntimeError, "Error while decode JPEG");
    return 0;
  }

  // TODO: set refcount somehow for image_buffer
  return PyMemoryView_FromMemory((char *)image, width * height * n_channels, PyBUF_READ);
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
