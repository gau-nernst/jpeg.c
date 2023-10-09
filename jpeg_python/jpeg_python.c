// https://docs.python.org/3/extending/extending.html
#include "jpeg.h"

#define PY_SSIZE_T_CLEAN
#include <Python.h>

static PyObject *jpeg_python_decode_jpeg(PyObject *self, PyObject *args) {
  char *filename = NULL;

  if (!PyArg_ParseTuple(args, "s", &filename))
    return NULL;

  FILE *f = fopen(filename, "rb");
  JPEGState jpeg_state;

  if (decode_jpeg(f, &jpeg_state)) {
    if (jpeg_state.components != NULL)
      free(jpeg_state.components);
    if (jpeg_state.image_buffer != NULL)
      free(jpeg_state.image_buffer);
    // TODO: set error code
    return NULL;
  }

  return PyMemoryView_FromMemory((char *)jpeg_state.image_buffer,
                                 jpeg_state.width * jpeg_state.height * jpeg_state.n_components, PyBUF_READ);
}

// method table
static PyMethodDef JpegPythonMethods[] = {
    {"decode_jpeg", jpeg_python_decode_jpeg, METH_VARARGS, "decode_jpeg docstring"}, {NULL, NULL, 0, NULL} // sentinel
};

// module definition
static struct PyModuleDef jpeg_module = {PyModuleDef_HEAD_INIT, "jpeg_python", "module docstring", -1,
                                         JpegPythonMethods};

// module initialization
PyMODINIT_FUNC PyInit_jpeg_python(void) {
  init_dct_matrix();
  return PyModule_Create(&jpeg_module);
}
