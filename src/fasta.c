#include "fasta.h"
#include "util.h"
#include "identifier.h"
#include "structmember.h"

/*calculate fasta attributes including sequence count, length,
composition (ATGCN count) and GC content
*/
void pyfastx_calc_fasta_attrs(pyfastx_Fasta *self){
	//ACGTN nucleotide counts
	int a, c, g, t, n;

	sqlite3_stmt *stmt;
	
	//sequence count
	sqlite3_prepare_v2(self->index->index_db, "SELECT COUNT(*) FROM seq LIMIT 1;", -1, &stmt, NULL);
	sqlite3_step(stmt);
	self->seq_counts = sqlite3_column_int(stmt, 0);
	sqlite3_reset(stmt);

	//sequence length
	sqlite3_prepare_v2(self->index->index_db, "SELECT SUM(slen) FROM seq LIMIT 1;", -1, &stmt, NULL);
	sqlite3_step(stmt);
	self->seq_length = sqlite3_column_int64(stmt, 0);
	sqlite3_reset(stmt);

	//calculate base counts
	sqlite3_prepare_v2(self->index->index_db, "SELECT SUM(a),SUM(c),SUM(g),SUM(t),SUM(n) FROM seq LIMIT 1;", -1, &stmt, NULL);
	sqlite3_step(stmt);
	a = sqlite3_column_int(stmt, 0);
	c = sqlite3_column_int(stmt, 1);
	g = sqlite3_column_int(stmt, 2);
	t = sqlite3_column_int(stmt, 3);
	n = sqlite3_column_int(stmt, 4);
	self->composition = Py_BuildValue("{s:i,s:i,s:i,s:i,s:i}", "A", a, "C", c, "G", g, "T", t, "N", n);
	sqlite3_finalize(stmt);

	//calc GC content
	self->gc_content = (float)(g+c)/(a+c+g+t)*100;
}


PyObject *pyfastx_fasta_new(PyTypeObject *type, PyObject *args, PyObject *kwargs){
	//fasta file path
	char *file_name;

	//bool value for uppercase sequence
	int uppercase = 1;

	//build index or not
	int build_index = 1;

	//paramters for fasta object construction
	static char* keywords[] = {"file_name", "uppercase", "build_index", NULL};
	
	if(!PyArg_ParseTupleAndKeywords(args, kwargs, "s|pp", keywords, &file_name, &uppercase, &build_index)){
		return NULL;
	}

	//check input sequence file is whether exists
	if(!file_exists(file_name)){
		return PyErr_Format(PyExc_FileExistsError, "input sequence file %s does not exists", file_name);
	}

	//create Fasta class
	pyfastx_Fasta *obj = (pyfastx_Fasta *)type->tp_alloc(type, 0);
	if (!obj){
		return NULL;
	}
	
	//initial sequence file name
	obj->file_name = (char *)malloc(strlen(file_name)+1);
	strcpy(obj->file_name, file_name);

	obj->uppercase = uppercase;

	//create index
	obj->index = pyfastx_init_index(obj->file_name, uppercase);

	//if build_index is True
	if(build_index){
		pyfastx_build_index(obj->index);
		pyfastx_calc_fasta_attrs(obj);
	}
	
	return (PyObject *)obj;
}

void pyfastx_fasta_dealloc(pyfastx_Fasta *self){
	pyfastx_index_free(self->index);
	Py_TYPE(self)->tp_free(self);
}

PyObject *pyfastx_fasta_iter(pyfastx_Fasta *self){
	pyfastx_rewind_index(self->index);
	Py_INCREF(self);
	return (PyObject *)self;
}

PyObject *pyfastx_fasta_repr(pyfastx_Fasta *self){
	return PyUnicode_FromFormat("<Fasta> %s contains %d seqs", self->file_name, self->seq_counts);
}

PyObject *pyfastx_fasta_next(pyfastx_Fasta *self){
	return pyfastx_get_next_seq(self->index);
}

PyObject *pyfastx_fasta_build_index(pyfastx_Fasta *self, PyObject *args, PyObject *kwargs){
	if(!file_exists(self->index->index_file)){
		pyfastx_build_index(self->index);
		pyfastx_calc_fasta_attrs(self);
	}
	Py_RETURN_NONE;
}

PyObject *pyfastx_fasta_rebuild_index(pyfastx_Fasta *self, PyObject *args, PyObject *kwargs){
	if(file_exists(self->index->index_file)){
		remove(self->index->index_file);
	}
	pyfastx_build_index(self->index);
	pyfastx_calc_fasta_attrs(self);
	Py_RETURN_NONE;
}

PyObject *pyfastx_fasta_fetch(pyfastx_Fasta *self, PyObject *args, PyObject *kwargs){
	static char* keywords[] = {"name", "intervals", "strand", NULL};

	char *name;
	char *seq;
	PyObject *intervals;
	char *strand = "+";
	int start;
	int end;
	
	if(!PyArg_ParseTupleAndKeywords(args, kwargs, "sO|s", keywords, &name, &intervals, &strand)){
		return NULL;
	}

	if(!PyTuple_Check(intervals) && !PyList_Check(intervals)){
		PyErr_SetString(PyExc_ValueError, "Intervals must be list or tuple");
		return NULL;
	}

	if(PyList_Check(intervals)){
		intervals = PyList_AsTuple(intervals);
	}

	PyObject *item;

	item = PyTuple_GetItem(intervals, 0);
	Py_ssize_t size = PyTuple_Size(intervals);

	// sqlite3 prepare object
	sqlite3_stmt *stmt;
	
	//select sql statement, seqid indicates seq name or chromomsome
	const char* sql = "SELECT * FROM seq WHERE seqid=? LIMIT 1;";
	sqlite3_prepare_v2(self->index->index_db, sql, -1, &stmt, NULL);
	sqlite3_bind_text(stmt, 1, name, -1, NULL);
	if(sqlite3_step(stmt) != SQLITE_ROW){
		return PyErr_Format(PyExc_NameError, "Sequence %s does not exists", name);
	}

	int seq_len;
	char *sub_seq;

	seq = pyfastx_index_get_full_seq(self->index, name);

	if(size == 2 && PyLong_Check(item)){
		start = PyLong_AsLong(item);
		end = PyLong_AsLong(PyTuple_GetItem(intervals, 1));

		if(start > end){
			PyErr_SetString(PyExc_ValueError, "Start position > end position");
			return NULL;
		}

		seq_len = end - start + 1;

		sub_seq = (char *)malloc(seq_len + 1);
		memcpy(sub_seq, seq+start-1, seq_len);
		sub_seq[seq_len] = '\0';
	} else {
		int i;
		int j = 0;
		sub_seq = (char *)malloc(strlen(seq) + 1);

		for(i=0; i<size; i++){
			item = PyTuple_GetItem(intervals, i);
			if(PyList_Check(item)){
				item = PyList_AsTuple(item);
			}
			start = PyLong_AsLong(PyTuple_GetItem(item, 0));
			end = PyLong_AsLong(PyTuple_GetItem(item, 1));
			seq_len = end - start + 1;

			if(start > end){
				PyErr_SetString(PyExc_ValueError, "Start position > end position");
				return NULL;
			}

			memcpy(sub_seq+j, seq+start-1, seq_len);
			j += seq_len;
		}
		sub_seq[j] = '\0';
	}

	if(strcmp(strand, "-") == 0){
		reverse_seq(sub_seq);
		complement_seq(sub_seq);
	}

	return Py_BuildValue("s", sub_seq);
}

PyObject *pyfastx_fasta_keys(pyfastx_Fasta *self, PyObject *args, PyObject *kwargs){
	pyfastx_Identifier *ids = PyObject_New(pyfastx_Identifier, &pyfastx_IdentifierType);
	if(!ids){
		return NULL;
	}

	ids->index_db = self->index->index_db;
	ids->stmt = NULL;
	ids->seq_counts = self->seq_counts;

	Py_INCREF(ids);
	return (PyObject *)ids;
}

PyObject *pyfastx_fasta_subscript(pyfastx_Fasta *self, PyObject *item){
	
	if (PyIndex_Check(item)) {
		Py_ssize_t i;
		i = PyNumber_AsSsize_t(item, PyExc_IndexError);

		if (i < 0) {
			i += self->seq_counts;
		}

		if(i >= self->seq_counts){
			PyErr_SetString(PyExc_IndexError, "index out of range");
			return NULL;
		}

		return pyfastx_index_get_seq_by_id(self->index, i+1);
		
	} else if (PyUnicode_CheckExact(item)) {
		char *key = PyUnicode_AsUTF8(item);

		return pyfastx_index_get_seq_by_name(self->index, key);

	} else {
		return NULL;
	}
}

int pyfastx_fasta_length(pyfastx_Fasta *self){
	return self->seq_counts;
}

int pyfastx_fasta_contains(pyfastx_Fasta *self, PyObject *key){
	sqlite3_stmt *stmt;
	
	char *name = PyUnicode_AsUTF8(key);

	sqlite3_prepare_v2(self->index->index_db, "SELECT * FROM seq WHERE seqid=? LIMIT 1;", -1, &stmt, NULL);
	sqlite3_bind_text(stmt, 1, name, -1, NULL);
	if(sqlite3_step(stmt) != SQLITE_ROW){
		return 0;
	}

	return 1;
}

PyObject *pyfastx_fasta_count(pyfastx_Fasta *self, PyObject *args){
	int l;
	int c;
	sqlite3_stmt *stmt;

	if (!PyArg_ParseTuple(args, "i", &l)) {
		return NULL;
	}

	const char *sql = "SELECT COUNT(*) FROM seq WHERE slen>=?";
	sqlite3_prepare_v2(self->index->index_db, sql, -1, &stmt, NULL);
	sqlite3_bind_int(stmt, 1, l);
	if (sqlite3_step(stmt) == SQLITE_ROW) {
		c = sqlite3_column_int(stmt, 0);
		sqlite3_finalize(stmt);
		return Py_BuildValue("i", c);
	}

	Py_RETURN_NONE;
}

PyObject *pyfastx_fasta_nl(pyfastx_Fasta *self, PyObject *args){
	sqlite3_stmt *stmt;
	int p;
	float half_size;
	int temp_size = 0;
	int i = 0;
	int j = 0;

	if (!PyArg_ParseTuple(args, "i", &p)) {
		return NULL;
	}

	if (p < 0 && p > 100){
		return PyErr_Format(PyExc_ValueError, "The value must between 0 and 100");
	}

	half_size = p/100.0 * self->seq_length;

	const char *sql = "SELECT slen FROM seq ORDER BY slen DESC";
	sqlite3_prepare_v2(self->index->index_db, sql, -1, &stmt, NULL);
	
	while (sqlite3_step(stmt) == SQLITE_ROW) {
		i++;
		j = sqlite3_column_int(stmt, 0);
		temp_size += j;
		if (temp_size >= half_size) {
			sqlite3_finalize(stmt);
			return Py_BuildValue("ii", j, i);
		}
	}
	Py_RETURN_NONE;
}

PyObject *pyfastx_fasta_longest(pyfastx_Fasta *self, void* closure){
	sqlite3_stmt *stmt;
	char *name;
	int len;
	const char *sql = "SELECT seqid,MAX(slen) FROM seq LIMIT 1";
	sqlite3_prepare_v2(self->index->index_db, sql, -1, &stmt, NULL);

	if (sqlite3_step(stmt) == SQLITE_ROW) {
		name = (char *)sqlite3_column_text(stmt, 0);
		len = sqlite3_column_int(stmt, 1);
		sqlite3_finalize(stmt);
		return Py_BuildValue("si", name, len);
	}

	Py_RETURN_NONE;
}

PyObject *pyfastx_fasta_shortest(pyfastx_Fasta *self, void* closure){
	sqlite3_stmt *stmt;
	char *name;
	int len;
	const char *sql = "SELECT seqid,MIN(slen) FROM seq LIMIT 1";
	sqlite3_prepare_v2(self->index->index_db, sql, -1, &stmt, NULL);

	if (sqlite3_step(stmt) == SQLITE_ROW) {
		name = (char *)sqlite3_column_text(stmt, 0);
		len = sqlite3_column_int(stmt, 1);
		sqlite3_finalize(stmt);
		return Py_BuildValue("si", name, len);
	}

	Py_RETURN_NONE;
}

PyObject *pyfastx_fasta_mean(pyfastx_Fasta *self, void* closure){
	sqlite3_stmt *stmt;
	int len;
	const char *sql = "SELECT AVG(slen) FROM seq LIMIT 1";
	sqlite3_prepare_v2(self->index->index_db, sql, -1, &stmt, NULL);

	if (sqlite3_step(stmt) == SQLITE_ROW) {
		len = sqlite3_column_int(stmt, 0);
		sqlite3_finalize(stmt);
		return Py_BuildValue("i", len);
	}

	Py_RETURN_NONE;
}

PyObject *pyfastx_fasta_median(pyfastx_Fasta *self, void* closure){
	sqlite3_stmt *stmt;
	int m;
	const char *sql;
	if (self->seq_counts % 2 == 0) {
		sql = "SELECT AVG(slen) FROM seq LIMIT ?,2";
	} else {
		sql = "SELECT slen FROM seq LIMIT ?,1";
	}
	sqlite3_prepare_v2(self->index->index_db, sql, -1, &stmt, NULL);
	sqlite3_bind_int(stmt, 1, (self->seq_counts - 1)/2);
	if(sqlite3_step(stmt) == SQLITE_ROW){
		m = sqlite3_column_int(stmt, 0);
		sqlite3_finalize(stmt);
		return Py_BuildValue("i", m);
	}

	Py_RETURN_NONE;
}


static PyGetSetDef pyfastx_fasta_getsets[] = {
	{"longest", (getter)pyfastx_fasta_longest, NULL, NULL, NULL},
	{"shortest", (getter)pyfastx_fasta_shortest, NULL, NULL, NULL},
	{"mean", (getter)pyfastx_fasta_mean, NULL, NULL, NULL},
	{"median", (getter)pyfastx_fasta_median, NULL, NULL, NULL},
	{NULL}
};

static PyMemberDef pyfastx_fasta_members[] = {
	{"file_name", T_STRING, offsetof(pyfastx_Fasta, file_name), READONLY},
	{"size", T_LONG, offsetof(pyfastx_Fasta, seq_length), READONLY},
	//{"count", T_INT, offsetof(pyfastx_Fasta, seq_counts), READONLY},
	{"gc_content", T_FLOAT, offsetof(pyfastx_Fasta, gc_content), READONLY},
	{"composition", T_OBJECT, offsetof(pyfastx_Fasta, composition), READONLY},
	{NULL}
};

static PyMethodDef pyfastx_fasta_methods[] = {
	{"build_index", (PyCFunction)pyfastx_fasta_build_index, METH_VARARGS},
	{"rebuild_index", (PyCFunction)pyfastx_fasta_rebuild_index, METH_VARARGS},
	{"fetch", (PyCFunction)pyfastx_fasta_fetch, METH_VARARGS|METH_KEYWORDS},
	{"count", (PyCFunction)pyfastx_fasta_count, METH_VARARGS},
	{"keys", (PyCFunction)pyfastx_fasta_keys, METH_VARARGS},
	{"nl", (PyCFunction)pyfastx_fasta_nl, METH_VARARGS},
	//{"test", (PyCFunction)test, METH_VARARGS},
	{NULL, NULL, 0, NULL}
};

//as a list
static PySequenceMethods seq_methods = {
	0, /*sq_length*/
	0, /*sq_concat*/
	0, /*sq_repeat*/
	0, /*sq_item*/
	0, /*sq_slice */
	0, /*sq_ass_item*/
	0, /*sq_ass_splice*/
	(objobjproc)pyfastx_fasta_contains, /*sq_contains*/
	0, /*sq_inplace_concat*/
	0, /*sq_inplace_repeat*/
};

static PyMappingMethods pyfastx_fasta_as_mapping = {
	(lenfunc)pyfastx_fasta_length,
	(binaryfunc)pyfastx_fasta_subscript,
	0,
};

PyTypeObject pyfastx_FastaType = {
    PyVarObject_HEAD_INIT(NULL, 0)
    "Fasta",                        /* tp_name */
    sizeof(pyfastx_Fasta),          /* tp_basicsize */
    0,                              /* tp_itemsize */
    (destructor)pyfastx_fasta_dealloc,   /* tp_dealloc */
    0,                              /* tp_print */
    0,                              /* tp_getattr */
    0,                              /* tp_setattr */
    0,                              /* tp_reserved */
    (reprfunc)pyfastx_fasta_repr,                              /* tp_repr */
    0,                              /* tp_as_number */
    &seq_methods,                   /* tp_as_sequence */
    &pyfastx_fasta_as_mapping,                   /* tp_as_mapping */
    0,                              /* tp_hash */
    0,                              /* tp_call */
    0,                              /* tp_str */
    0,                              /* tp_getattro */
    0,                              /* tp_setattro */
    0,                              /* tp_as_buffer */
    Py_TPFLAGS_DEFAULT,             /* tp_flags */
    0,                              /* tp_doc */
    0,                              /* tp_traverse */
    0,                              /* tp_clear */
    0,                              /* tp_richcompare */
    0,                              /* tp_weaklistoffset */
    (getiterfunc)pyfastx_fasta_iter,     /* tp_iter */
    (iternextfunc)pyfastx_fasta_next,    /* tp_iternext */
    pyfastx_fasta_methods,          /* tp_methods */
    pyfastx_fasta_members,          /* tp_members */
    pyfastx_fasta_getsets,                              /* tp_getset */
    0,                              /* tp_base */
    0,                              /* tp_dict */
    0,                              /* tp_descr_get */
    0,                              /* tp_descr_set */
    0,                              /* tp_dictoffset */
    0,                              /* tp_init */
    PyType_GenericAlloc,            /* tp_alloc */
    pyfastx_fasta_new,              /* tp_new */
};
