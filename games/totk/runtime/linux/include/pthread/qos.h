/* Linux: Darwin QoS classes. USER_INTERACTIVE raises nothing (no RT without privileges); the class is remembered per thread. */
#pragma once
#include "darwin_compat.h"
typedef enum { QOS_CLASS_USER_INTERACTIVE = 0x21, QOS_CLASS_USER_INITIATED = 0x19, QOS_CLASS_DEFAULT = 0x15, QOS_CLASS_UTILITY = 0x11,
               QOS_CLASS_BACKGROUND = 0x09, QOS_CLASS_UNSPECIFIED = 0 } qos_class_t;
extern __thread qos_class_t darwin_qos_class;
static inline int pthread_set_qos_class_self_np(qos_class_t q, int rel) { (void)rel; darwin_qos_class = q; return 0; }
static inline int pthread_get_qos_class_np(pthread_t t, qos_class_t *q, int *rel) { (void)t; *q = darwin_qos_class; if (rel) *rel = 0; return 0; }
static inline int pthread_attr_set_qos_class_np(pthread_attr_t *a, qos_class_t q, int rel) { (void)a; (void)q; (void)rel; return 0; }
