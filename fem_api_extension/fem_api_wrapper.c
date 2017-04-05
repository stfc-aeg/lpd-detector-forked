#include <Python.h>

#if PY_MAJOR_VERSION >= 3
  #define MOD_ERROR_VAL NULL
  #define MOD_SUCCESS_VAL(val) val
  #define MOD_INIT(name) PyMODINIT_FUNC PyInit_##name(void)
  #define MOD_DEF(ob, name, doc, methods) \
          static struct PyModuleDef moduledef = { \
            PyModuleDef_HEAD_INIT, name, doc, -1, methods, }; \
          ob = PyModule_Create(&moduledef);

  #define PyInt_FromLong PyLong_FromLong
  #define PyInt_AsLong   PyLong_AsLong
  #define PyInt_Check    PyLong_Check

#else
  #define MOD_ERROR_VAL
  #define MOD_SUCCESS_VAL(val)
  #define MOD_INIT(name) void init##name(void)
  #define MOD_DEF(ob, name, doc, methods) \
          ob = Py_InitModule3(name, methods, doc);
#endif

#if PY_MAJOR_VERSION >= 3
#endif

#include <stdio.h>
#include <stdarg.h>
#include <stdlib.h>

#include "femApi.h"

/*
 * A simple structure to be used as an opaque pointer to a FEM object in API
 * calls. This wraps the real handle, which can then be set to NULL if the
 * user explicitly calls close(), and tested in methods accordingly.
*/
typedef struct Fem {
    void* handle;
    CtlConfig config;
} Fem;
typedef Fem* FemPtr;

/* Forward declarations */
static void _del(PyObject* obj);

/* An exception object to be raised by the API wrapper */
static PyObject* fem_api_error;

/* Helper function to format the API exception error message using printf() style arguments */
#define MAX_ERR_STRING_LEN 128
void _set_api_error_string(const char* format, ...) {
    char err_str[MAX_ERR_STRING_LEN];
    va_list arglist;
    va_start(arglist, format);
    vsnprintf(err_str, MAX_ERR_STRING_LEN, format, arglist);
    va_end(arglist);
    PyErr_SetString(fem_api_error, err_str);
}

/* Helper to validate the opaque FEM pointer object and the handle it contains */
#define _validate_ptr_and_handle(ptr, func_name) \
    if (ptr == NULL) { \
        _set_api_error_string("%s: resolved FEM object pointer to null", func_name); \
        return NULL; } \
    if (ptr->handle == NULL) { \
        _set_api_error_string("%s: FEM object pointer has null FEM handle", func_name); \
        return NULL; }


typedef enum log_level_ {info, warning, error, debug} log_level;
#define MAX_LOG_STRING_LEN 128

static void log_msg(log_level level, const char* format, ...)
{
    static PyObject *logging = NULL;
    static PyObject *string = NULL;

    // Import logging module when needed
    if (logging == NULL){
        logging = PyImport_ImportModuleNoBlock("logging");
        if (logging == NULL)
            PyErr_SetString(PyExc_ImportError,
                "Could not import module 'logging'");
    }

    // Build the log message from the format and variable argument list
    char msg[MAX_LOG_STRING_LEN];
    va_list arglist;
    va_start(arglist, format);
    vsnprintf(msg, MAX_LOG_STRING_LEN, format, arglist);
    va_end(arglist);

    string = Py_BuildValue("s", msg);

    // Call the logging function depending on loglevel
    switch (level)
    {
        case info:
            PyObject_CallMethod(logging, "info", "O", string);
            break;

        case warning:
            PyObject_CallMethod(logging, "warn", "O", string);
            break;

        case error:
            PyObject_CallMethod(logging, "error", "O", string);
            break;

        case debug:
            PyObject_CallMethod(logging, "debug", "O", string);
            break;
    }
    Py_DECREF(string);
}

static PyObject* _initialise(PyObject* self, PyObject* args)
{
    int fem_id;
    char* fem_address;
    int fem_port;
    char* data_address;
    int rc;

    if (!PyArg_ParseTuple(args, "isis", &fem_id, &fem_address, &fem_port,
                          &data_address)) {
    	PyErr_SetString(fem_api_error, "Incorrect arguments passed to initialise FEM API");
        return NULL;
    }

    FemPtr fem_ptr = malloc(sizeof(Fem));
    if (fem_ptr == NULL) {
        PyErr_SetString(fem_api_error, "Unable to malloc() space for FEM object");
        return NULL;
    }

    /* Set up the CtlConfig structure to pass to the FEM on initialisation */
    fem_ptr->config.femNumber = fem_id;
    fem_ptr->config.femAddress = fem_address;
    fem_ptr->config.femPort = fem_port;
    fem_ptr->config.dataAddress = data_address;

    Py_BEGIN_ALLOW_THREADS
    rc = femInitialise((void*)NULL, (const CtlCallbacks*)NULL, (const CtlConfig*)&(fem_ptr->config), &(fem_ptr->handle));
    Py_END_ALLOW_THREADS

    if (rc != FEM_RTN_OK) {
        PyErr_SetString(fem_api_error, femErrorMsg(fem_ptr->handle));
        return NULL;
    }

    //printf("Initialised module with handle %lu\n", (unsigned long)(fem_ptr->handle));
    log_msg(debug, "Initialised fem_api module with handle %lu for FEM ID %d",
        (unsigned long)(fem_ptr->handle), fem_id);

    return PyCapsule_New(fem_ptr, "FemPtr", _del);
}

static PyObject* _get_id(PyObject* self, PyObject* args)
{

    PyObject* _handle;
    FemPtr fem_ptr;
    int id;

    if (!PyArg_ParseTuple(args, "O", &_handle)) {
        return NULL;
    }

    fem_ptr = (FemPtr) PyCapsule_GetPointer(_handle, "FemPtr");
    _validate_ptr_and_handle(fem_ptr, "get_id");

    id = femGetId(fem_ptr->handle);
    return Py_BuildValue("i", id);
}

static PyObject* _get_int(PyObject* self, PyObject* args)
{
    int rc;
    PyObject* _handle;
    int chip_id, param_id, size;
    FemPtr fem_ptr;

    unsigned int* value_ptr;
    PyObject* values;

    if (!PyArg_ParseTuple(args, "Oiii", &_handle, &chip_id, &param_id, &size)) {
        return NULL;
    }
    // printf("_get_int: chip_id %d param_id %d size %d\n", chip_id, param_id, size);

    fem_ptr = (FemPtr) PyCapsule_GetPointer(_handle, "FemPtr");
    _validate_ptr_and_handle(fem_ptr, "get_int");

    value_ptr = (unsigned int *)malloc(size * sizeof(unsigned int));
    if (value_ptr == NULL) {
        _set_api_error_string("get_int: unable to allocate space for %d integer values", size);
        return NULL;
    }

    Py_BEGIN_ALLOW_THREADS
    rc = femGetInt(fem_ptr->handle, chip_id, param_id, size, (int *)(value_ptr));
    Py_END_ALLOW_THREADS

    values = PyList_New(size);
    if (rc == FEM_RTN_OK) {
        int ival;
        for (ival = 0; ival < size; ival++) {
            PyList_SetItem(values, ival, PyInt_FromLong(value_ptr[ival]));
        }
    }
    free(value_ptr);

    return Py_BuildValue("iO", rc, values);
}

static PyObject* _set_int(PyObject* self, PyObject* args)
{
    int rc;
    PyObject* _handle;
    int chip_id, param_id, size;
    PyObject* values_obj;
    FemPtr fem_ptr;

    int* value_ptr = NULL;
    int single_value = 0;

    if (!PyArg_ParseTuple(args, "OiiO", &_handle, &chip_id, &param_id, &values_obj)) {
        return NULL;
    }

    fem_ptr = (FemPtr) PyCapsule_GetPointer(_handle, "FemPtr");
    _validate_ptr_and_handle(fem_ptr, "set_int");

    if (PyInt_Check(values_obj)) {
        size = 1;
        single_value = 1;
    }
    else if(PyList_Check(values_obj)) {
        size = PyList_Size(values_obj);
        single_value = 0;
    } else {
        _set_api_error_string("set_int: specified value(s) not int or list");
        return NULL;
    }

    value_ptr = (int *)malloc(size * sizeof(int));
    if (value_ptr == NULL) {
        _set_api_error_string("set_int: unable to allocate space for %d integer values", size);
        return NULL;
    }

    if (single_value == 1) {
        *value_ptr = PyInt_AsLong(values_obj);
    } else {
        int ival;
        for (ival = 0; ival < size; ival++) {
            PyObject* value_obj = PyList_GetItem(values_obj, ival);
            if (!PyInt_Check(value_obj)) {
                _set_api_error_string("set_int: non-integer value specified");
                free(value_ptr);
                return NULL;
            }
            value_ptr[ival] = PyInt_AsLong(PyList_GetItem(values_obj, ival));
        }
    }
    Py_BEGIN_ALLOW_THREADS
    rc = femSetInt(fem_ptr->handle, chip_id, param_id, size, value_ptr);
    Py_END_ALLOW_THREADS

    if (value_ptr != NULL) {
        free(value_ptr);
    }

    return Py_BuildValue("i", rc);
}

static PyObject* _get_short(PyObject* self, PyObject* args)
{
    int rc;
    PyObject* _handle;
    int chip_id, param_id, size;
    FemPtr fem_ptr;

    short* value_ptr;
    PyObject* values;

    if (!PyArg_ParseTuple(args, "Oiii", &_handle, &chip_id, &param_id, &size)) {
        return NULL;
    }
    //printf("_get_int: chip_id %d param_id %d size %d\n", chip_id, param_id, size);

    fem_ptr = (FemPtr) PyCapsule_GetPointer(_handle, "FemPtr");
    _validate_ptr_and_handle(fem_ptr, "get_short");

    value_ptr = (short *)malloc(size * sizeof(short));
    if (value_ptr == NULL) {
        _set_api_error_string("get_short: unable to allocate space for %d short values", size);
        return NULL;
    }

    Py_BEGIN_ALLOW_THREADS
    rc = femGetShort(fem_ptr->handle, chip_id, param_id, size, value_ptr);
    Py_END_ALLOW_THREADS

    values = PyList_New(size);
    if (rc == FEM_RTN_OK) {
        int ival;
        for (ival = 0; ival < size; ival++) {
            PyList_SetItem(values, ival, PyInt_FromLong((long)value_ptr[ival]));
        }
    }
    free(value_ptr);

    return Py_BuildValue("iO", rc, values);
}

static PyObject* _set_short(PyObject* self, PyObject* args)
{
    int rc;
    PyObject* _handle;
    int chip_id, param_id, size;
    PyObject* values_obj;
    FemPtr fem_ptr;

    short* value_ptr = NULL;

    if (!PyArg_ParseTuple(args, "OiiO", &_handle, &chip_id, &param_id, &values_obj)) {
        return NULL;
    }

    fem_ptr = (FemPtr) PyCapsule_GetPointer(_handle, "FemPtr");
    _validate_ptr_and_handle(fem_ptr, "set_short");

    if (PyInt_Check(values_obj)) {
        size = 1;
    }
    else if(PyList_Check(values_obj)) {
        size = PyList_Size(values_obj);
    } else {
        _set_api_error_string("set_short: specified value(s) not int or list");
        return NULL;
    }

    value_ptr = (short *)malloc(size * sizeof(short));
    if (value_ptr == NULL) {
        _set_api_error_string("set_short: unable to allocate space for %d short values", size);
        return NULL;
    }

    if (size == 1) {
        *value_ptr = (short)PyInt_AsLong(values_obj);
    } else {
        int ival;
        for (ival = 0; ival < size; ival++) {
            PyObject* value_obj = PyList_GetItem(values_obj, ival);
            if (!PyInt_Check(value_obj)) {
                _set_api_error_string("set_short: non-integer value specified");
                free(value_ptr);
                return NULL;
            }
            value_ptr[ival] = (short)PyInt_AsLong(PyList_GetItem(values_obj, ival));
        }
    }

    Py_BEGIN_ALLOW_THREADS
    rc = femSetShort(fem_ptr->handle, chip_id, param_id, size, value_ptr);
    Py_END_ALLOW_THREADS

    if (value_ptr != NULL) {
        free(value_ptr);
    }

    return Py_BuildValue("i", rc);
}

static PyObject* _get_float(PyObject* self, PyObject* args)
{
    int rc;
    PyObject* _handle;
    int chip_id, param_id, size;
    FemPtr fem_ptr;

    double* value_ptr;
    PyObject* values;

    if (!PyArg_ParseTuple(args, "Oiii", &_handle, &chip_id, &param_id, &size)) {
        return NULL;
    }
    //printf("_get_int: chip_id %d param_id %d size %d\n", chip_id, param_id, size);

    fem_ptr = (FemPtr) PyCapsule_GetPointer(_handle, "FemPtr");
    _validate_ptr_and_handle(fem_ptr, "get_int");

    value_ptr = (double *)malloc(size * sizeof(double));
    if (value_ptr == NULL) {
        _set_api_error_string("get_float: unable to allocate space for %d float values", size);
        return NULL;
    }

    Py_BEGIN_ALLOW_THREADS
    rc = femGetFloat(fem_ptr->handle, chip_id, param_id, size, (double *)(value_ptr));
    Py_END_ALLOW_THREADS

    values = PyList_New(size);
    if (rc == FEM_RTN_OK) {
        int ival;
        for (ival = 0; ival < size; ival++) {
            PyList_SetItem(values, ival, PyFloat_FromDouble(value_ptr[ival]));
        }
    }
    free(value_ptr);

    return Py_BuildValue("iO", rc, values);
}

static PyObject* _set_float(PyObject* self, PyObject* args)
{
    int rc;
    PyObject* _handle;
    int chip_id, param_id, size;
    PyObject* values_obj;
    FemPtr fem_ptr;

    double* value_ptr = NULL;

    if (!PyArg_ParseTuple(args, "OiiO", &_handle, &chip_id, &param_id, &values_obj)) {
        return NULL;
    }

    fem_ptr = (FemPtr) PyCapsule_GetPointer(_handle, "FemPtr");
    _validate_ptr_and_handle(fem_ptr, "set_float");

    if (PyInt_Check(values_obj)) {
        size = 1;
    }
    else if(PyList_Check(values_obj)) {
        size = PyList_Size(values_obj);
    } else {
        _set_api_error_string("set_float: specified value(s) not float or list");
        return NULL;
    }

    value_ptr = (double *)malloc(size * sizeof(double));
    if (value_ptr == NULL) {
        _set_api_error_string("set_float: unable to allocate space for %d float values", size);
        return NULL;
    }

    if (size == 1) {
        *value_ptr =PyFloat_AsDouble(values_obj);
    } else {
        int ival;
        for (ival = 0; ival < size; ival++) {
            PyObject* value_obj = PyList_GetItem(values_obj, ival);
            if (!PyFloat_Check(value_obj)) {
                _set_api_error_string("set_float: non-float value specified");
                free(value_ptr);
                return NULL;
            }
            value_ptr[ival] = (short)PyFloat_AsDouble(PyList_GetItem(values_obj, ival));
        }
    }

    Py_BEGIN_ALLOW_THREADS
    rc = femSetFloat(fem_ptr->handle, chip_id, param_id, size, value_ptr);
    Py_END_ALLOW_THREADS

    if (value_ptr != NULL) {
        free(value_ptr);
    }

    return Py_BuildValue("i", rc);
}

static PyObject* _cmd(PyObject* self, PyObject* args)
{
    PyObject* _handle;
    FemPtr fem_ptr;
    int chipId, cmdId;
    int rc;

    if (!PyArg_ParseTuple(args, "Oii", &_handle, &chipId, &cmdId)) {
        return NULL;
    }

    fem_ptr = (FemPtr) PyCapsule_GetPointer(_handle, "FemPtr");
    _validate_ptr_and_handle(fem_ptr, "cmd");

    Py_BEGIN_ALLOW_THREADS
    rc = femCmd(fem_ptr->handle, chipId, cmdId);
    Py_END_ALLOW_THREADS

    return Py_BuildValue("i", rc);
}

static PyObject* _close(PyObject* self, PyObject* args)
{
    PyObject* _handle;
    FemPtr fem_ptr;

    if (!PyArg_ParseTuple(args, "O", &_handle)) {
        return NULL;
    }

    fem_ptr = (FemPtr) PyCapsule_GetPointer(_handle, "FemPtr");
    _validate_ptr_and_handle(fem_ptr, "close");

    femClose(fem_ptr->handle);
    fem_ptr->handle = NULL;

    return Py_BuildValue("");
}

static PyObject* _get_error_msg(PyObject* self, PyObject* args)
{
    PyObject* _handle;
    FemPtr fem_ptr;
    char* error_str;

    if (!PyArg_ParseTuple(args, "O", &_handle)) {
        return NULL;
    }

    fem_ptr = (FemPtr) PyCapsule_GetPointer(_handle, "FemPtr");
    _validate_ptr_and_handle(fem_ptr, "get_error_str");

    error_str = (char*)femErrorMsg(fem_ptr->handle);

    return Py_BuildValue("s", error_str);

}

static void _del(PyObject* obj)
{
    FemPtr fem_ptr = (FemPtr) PyCapsule_GetPointer(obj, "FemPtr");
    if (fem_ptr == NULL) {
        return;
    }

    if (fem_ptr->handle != NULL) {
        femClose(fem_ptr->handle);
    }
}

/*  define functions in module */
static PyMethodDef fem_api_methods[] =
{
     {"initialise", _initialise, METH_VARARGS, "initialise a module"},
     {"get_id",     _get_id,     METH_VARARGS, "get a module ID"},
     {"get_int",    _get_int,    METH_VARARGS, "get one or more integer parameters"},
     {"set_int",    _set_int,    METH_VARARGS, "set one or more integer parameters"},
	 {"get_short",  _get_short,  METH_VARARGS, "get one ore more short integer parameters"},
	 {"set_short",  _set_short,  METH_VARARGS, "set one ore more short integer parameters"},
	 {"get_float",  _get_float,  METH_VARARGS, "get one or more float parameters"},
	 {"set_float",  _set_float,  METH_VARARGS, "set one or more float parameters"},
	 {"get_error_msg", _get_error_msg, METH_VARARGS, "get module error message"},
     {"cmd",        _cmd,        METH_VARARGS, "issue a command to a module"},
     {"close",      _close,      METH_VARARGS, "close a module"},
     {NULL, NULL, 0, NULL}
};

/* module initialization */
#ifdef COMPILE_AS_STUB
MOD_INIT(fem_api_stub)
#else
MOD_INIT(fem_api)
#endif
{
    PyObject* m;
#ifdef COMPILE_AS_STUB
    MOD_DEF(m, "fem_api_stub", "Module docstring", fem_api_methods)
#else
    MOD_DEF(m, "fem_api", "Module docstring", fem_api_methods)
#endif
    if (m == NULL) {
        return MOD_ERROR_VAL;
    }

    fem_api_error = PyErr_NewException("fem_api.error", NULL, NULL);
    Py_INCREF(fem_api_error);
    PyModule_AddObject(m, "error", fem_api_error);

    return MOD_SUCCESS_VAL(m);

}