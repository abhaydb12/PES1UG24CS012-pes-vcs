#ifndef OBJECT_H
#define OBJECT_H

#include "pes.h"

int object_write(ObjectType type, const void *data, size_t len, ObjectID *id_out);

#endif
