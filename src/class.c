/*
 * class.c - class metaobject implementation
 *
 *  Copyright(C) 2000-2001 by Shiro Kawai (shiro@acm.org)
 *
 *  Permission to use, copy, modify, distribute this software and
 *  accompanying documentation for any purpose is hereby granted,
 *  provided that existing copyright notices are retained in all
 *  copies and that this notice is included verbatim in all
 *  distributions.
 *  This software is provided as is, without express or implied
 *  warranty.  In no circumstances the author(s) shall be liable
 *  for any damages arising out of the use of this software.
 *
 *  $Id: class.c,v 1.57 2001-09-24 11:32:00 shirok Exp $
 */

#include "gauche.h"
#include "gauche/macro.h"
#include "gauche/class.h"

/*===================================================================
 * Built-in classes
 */

static void class_print(ScmObj, ScmPort *, ScmWriteContext*);
static void generic_print(ScmObj, ScmPort *, ScmWriteContext*);
static void method_print(ScmObj, ScmPort *, ScmWriteContext*);
static void next_method_print(ScmObj, ScmPort *, ScmWriteContext*);
static void slot_accessor_print(ScmObj, ScmPort *, ScmWriteContext*);

static ScmObj class_allocate(ScmClass *klass, ScmObj initargs);
static ScmObj generic_allocate(ScmClass *klass, ScmObj initargs);
static ScmObj method_allocate(ScmClass *klass, ScmObj initargs);
static ScmObj object_allocate(ScmClass *k, ScmObj initargs);
static ScmObj slot_accessor_allocate(ScmClass *klass, ScmObj initargs);
static void scheme_slot_default(ScmObj obj);

static ScmObj builtin_initialize(ScmObj *, int, ScmGeneric *);

ScmClass *Scm_DefaultCPL[] = { SCM_CLASS_TOP, NULL };
ScmClass *Scm_CollectionCPL[] = {
    SCM_CLASS_COLLECTION, SCM_CLASS_TOP, NULL
};
ScmClass *Scm_SequenceCPL[] = {
    SCM_CLASS_SEQUENCE, SCM_CLASS_COLLECTION, SCM_CLASS_TOP, NULL
};
ScmClass *Scm_ObjectCPL[] = {
    SCM_CLASS_OBJECT, SCM_CLASS_TOP, NULL
};

SCM_DEFINE_ABSTRACT_CLASS(Scm_TopClass, NULL);
SCM_DEFINE_ABSTRACT_CLASS(Scm_CollectionClass, SCM_CLASS_DEFAULT_CPL);
SCM_DEFINE_ABSTRACT_CLASS(Scm_SequenceClass, SCM_CLASS_COLLECTION_CPL);

SCM_DEFINE_BUILTIN_CLASS_SIMPLE(Scm_BoolClass, NULL);
SCM_DEFINE_BUILTIN_CLASS_SIMPLE(Scm_CharClass, NULL);
SCM_DEFINE_BUILTIN_CLASS_SIMPLE(Scm_UnknownClass, NULL);

SCM_DEFINE_BASE_CLASS(Scm_ObjectClass, ScmObj,
                      NULL, NULL, NULL, object_allocate,
                      SCM_CLASS_DEFAULT_CPL);

/* Those basic metaobjects will be initialized further in Scm__InitClass */
SCM_DEFINE_BASE_CLASS(Scm_ClassClass, ScmClass,
                      class_print, NULL, NULL, class_allocate,
                      SCM_CLASS_OBJECT_CPL);
SCM_DEFINE_BASE_CLASS(Scm_GenericClass, ScmGeneric,
                      generic_print, NULL, NULL, generic_allocate,
                      SCM_CLASS_OBJECT_CPL);
SCM_DEFINE_BASE_CLASS(Scm_MethodClass, ScmMethod,
                      method_print, NULL, NULL, method_allocate,
                      SCM_CLASS_OBJECT_CPL);

/* Internally used classes */
SCM_DEFINE_BUILTIN_CLASS(Scm_SlotAccessorClass,
                         slot_accessor_print, NULL, NULL,
                         slot_accessor_allocate,
                         SCM_CLASS_DEFAULT_CPL);
SCM_DEFINE_BUILTIN_CLASS_SIMPLE(Scm_NextMethodClass, next_method_print);

/* Builtin generic functions */
SCM_DEFINE_GENERIC(Scm_GenericMake, Scm_NoNextMethod, NULL);
SCM_DEFINE_GENERIC(Scm_GenericAllocate, Scm_NoNextMethod, NULL);
SCM_DEFINE_GENERIC(Scm_GenericInitialize, builtin_initialize, NULL);
SCM_DEFINE_GENERIC(Scm_GenericAddMethod, Scm_NoNextMethod, NULL);
SCM_DEFINE_GENERIC(Scm_GenericComputeCPL, Scm_NoNextMethod, NULL);
SCM_DEFINE_GENERIC(Scm_GenericComputeSlots, Scm_NoNextMethod, NULL);
SCM_DEFINE_GENERIC(Scm_GenericComputeGetNSet, Scm_NoNextMethod, NULL);
SCM_DEFINE_GENERIC(Scm_GenericComputeApplicableMethods, Scm_NoNextMethod, NULL);
SCM_DEFINE_GENERIC(Scm_GenericApplyGeneric, Scm_NoNextMethod, NULL);
SCM_DEFINE_GENERIC(Scm_GenericMethodMoreSpecificP, Scm_NoNextMethod, NULL);
SCM_DEFINE_GENERIC(Scm_GenericSlotMissing, Scm_NoNextMethod, NULL);
SCM_DEFINE_GENERIC(Scm_GenericSlotUnbound, Scm_NoNextMethod, NULL);
SCM_DEFINE_GENERIC(Scm_GenericSlotBoundUsingClassP, Scm_NoNextMethod, NULL);

/* Some frequently-used pointers */
static ScmObj key_allocation;
static ScmObj key_instance;
static ScmObj key_accessor;
static ScmObj key_slot_accessor;
static ScmObj key_builtin;
static ScmObj key_name;
static ScmObj key_supers;
static ScmObj key_slots;
static ScmObj key_metaclass;
static ScmObj key_lambda_list;
static ScmObj key_generic;
static ScmObj key_specializers;
static ScmObj key_body;
static ScmObj key_init_keyword;
static ScmObj key_init_thunk;
static ScmObj key_init_value;
static ScmObj key_slot_num;
static ScmObj key_slot_set;
static ScmObj key_slot_ref;

/*=====================================================================
 * Auxiliary utilities
 */

static ScmClass **class_list_to_array(ScmObj classes, int len)
{
    ScmObj cp;
    ScmClass **v, **vp;
    v = vp = SCM_NEW2(ScmClass**, sizeof(ScmClass*)*(len+1));
    SCM_FOR_EACH(cp, classes) {
        if (!Scm_TypeP(SCM_CAR(cp), SCM_CLASS_CLASS))
            Scm_Error("list of classes required, but found non-class object"
                      " %S in %S", SCM_CAR(cp), classes);
        *vp++ = SCM_CLASS(SCM_CAR(cp));
    }
    *vp = NULL;
    return v;
}

static ScmObj class_array_to_list(ScmClass **array, int len)
{
    ScmObj h = SCM_NIL, t = SCM_NIL;
    if (array) while (len-- > 0) SCM_APPEND1(h, t, SCM_OBJ(*array++));
    return h;
}

static ScmObj class_array_to_names(ScmClass **array, int len)
{
    ScmObj h = SCM_NIL, t = SCM_NIL;
    int i;
    for (i=0; i<len; i++, array++) SCM_APPEND1(h, t, (*array)->name);
    return h;
}

/*=====================================================================
 * Class metaobject
 */

/* One of the design goals of Gauche object system is to make Scheme-defined
 * class easily accessible from C code, and vice versa.
 *
 * Classes in Gauche fall into four categories: builtin class, base class,
 * abstract class and Scheme class.  A C-defined class may belong to one
 * of the first three, while a Scheme-defined class is always a Scheme class.
 *
 * Builtin classes are the ones that represents basic objects of the
 * language, such as <integer> or <string>.   Those classes are just
 * the way to reify the basic object, and don't follow object protorol;
 * for example, you can't overload "initialize" method specialized to
 * <integer> to customize initialization of integers, nor subclass <integer>,
 * although you can use them to specialize methods you write.
 *
 * Base classes are the ones from which you can derive Scheme classes.
 * <class>, <generic-method> and <method> are in this category.  The instance
 * of those classes may have extra slots that contains C-specific data, such
 * as function pointers.  You can subclass them, but there is one restriction:
 * There can't be more than one core base class in the class' superclasses.
 * Because of this fact, C routines can take the pointer to the instance
 * of subclasses and safely cast it to one of the core base classes.
 * (<object> is an only exception of this rule, so that <class> can inherit
 * <object>).
 *
 * Abstract classes are just for method specialization.  They can't
 * create instances directly, and they shouldn't have any direct slots.
 * <top>, <collection> and <sequence> are in this category, among others.
 *
 * Since a class must be <class> itself or its descendants, C code can
 * treat them as ScmClass*, and can determine the category of the class.
 *
 * Depending on its category, a class must or may provide those function
 * pointers:
 *
 *   Category:   base           builtin         abstract
 *   -----------------------------------------------------
 *    allocate   required       NULL            NULL
 *    print      optional       optional        ignored
 *    compare    optional       optional        ignored
 *    serialize  optional       optional        ignored
 *
 * If the function is optional, you can set NULL there and the system
 * uses default function.  For Scheme class the system sets appropriate
 * functions.   See the following section for details of these function
 * potiners.
 */

/*
 * Built-in protocols
 *
 *  ScmObj klass->allocate(ScmClass *klass, ScmObj initargs)
 *     Called at the bottom of the chain of allocate-instance method.
 *     Besides allocating the required space, it must initialize
 *     members of the C-specific part of the instance, including SCM_HEADER.
 *     This protocol can be NULL for core base classes; if so, attempt
 *     to "make" such class reports an error.
 *
 *  int klass->print(ScmObj obj, ScmPort *sink, int mode)
 *     OBJ is an instance of klass (you can safely assume it).  This
 *     function should print OBJ into SINK, and returns number of characters
 *     output to SINK.   MODE can be SCM_PRINT_DISPLAY for display(),
 *     SCM_PRINT_WRITE for write(), or SCM_PRINT_DEBUG for more precise
 *     debug information.
 *     If this function pointer is not set, a default print method
 *     is used.
 *
 *  int klass->compare(ScmObj x, ScmObj y)
 *     X and Y are instances of klass or its descendants.  If the objects
 *     are fully orderable, this function returns either -1, 0 or 1, depending
 *     X preceding Y, X being equal to Y, or X following Y, respectively.
 *     If the objects are not fully orderable, just returns 0.
 *     If this function pointer is not set, Gauche assumes objects are
 *     not orderable.
 *
 *  int klass->serialize(ScmObj obj, ScmPort *sink, ScmObj table)
 *     OBJ is an instance of klass.  This method is only called when OBJ
 *     has not been output in the current serializing session.
 */

/*
 * Class metaobject protocol implementation
 */

/* Allocate class structure.  klass is a metaclass. */
static ScmObj class_allocate(ScmClass *klass, ScmObj initargs)
{
    ScmClass *instance;
    int nslots = klass->numInstanceSlots;
    instance = SCM_NEW2(ScmClass*,
                        sizeof(ScmClass) + sizeof(ScmObj)*nslots);
    SCM_SET_CLASS(instance, klass);
    instance->allocate = NULL;  /* will be set when CPL is set */
    instance->print = NULL;
    instance->compare = NULL;
    instance->serialize = NULL; /* class_serialize? */
    instance->cpa = NULL;
    instance->numInstanceSlots = nslots;
    instance->instanceSlotOffset = 1; /* default */
    instance->flags = 0;        /* ?? */
    instance->name = SCM_FALSE;
    instance->directSupers = SCM_NIL;
    instance->accessors = SCM_NIL;
    instance->cpl = SCM_NIL;
    instance->directSlots = SCM_NIL;
    instance->slots = SCM_NIL;
    instance->directSubclasses = SCM_NIL;
    instance->directMethods = SCM_NIL;
    scheme_slot_default(SCM_OBJ(instance));
    return SCM_OBJ(instance);
}

static void class_print(ScmObj obj, ScmPort *port, ScmWriteContext *ctx) 
{
    Scm_Printf(port, "#<class %A>", SCM_CLASS(obj)->name);
}

/*
 * (make <class> ...)   - default method to make a class instance.
 */

/* defined in Scheme */

/*
 * (allocate-instance <class> initargs)
 */
static ScmObj allocate(ScmNextMethod *nm, ScmObj *args, int nargs, void *d)
{
    ScmClass *c = SCM_CLASS(args[0]);
    if (c->allocate == NULL) {
        Scm_Error("built-in class can't be allocated via allocate-instance: %S",
                  SCM_OBJ(c));
    }
    return c->allocate(c, args[1]);
}

static ScmClass *class_allocate_SPEC[] = { SCM_CLASS_CLASS, SCM_CLASS_LIST };
static SCM_DEFINE_METHOD(class_allocate_rec, &Scm_GenericAllocate,
                         2, 0, class_allocate_SPEC, allocate, NULL);

/*
 * (compute-cpl <class>)
 */
static ScmObj class_compute_cpl(ScmNextMethod *nm, ScmObj *args, int nargs,
                                void *d)
{
    ScmClass *c = SCM_CLASS(args[0]);
    return Scm_ComputeCPL(c);
}

static ScmClass *class_compute_cpl_SPEC[] = { SCM_CLASS_CLASS };
static SCM_DEFINE_METHOD(class_compute_cpl_rec, &Scm_GenericComputeCPL,
                         1, 0, class_compute_cpl_SPEC,
                         class_compute_cpl, NULL);

/*
 * Get class
 */

ScmClass *Scm_ClassOf(ScmObj obj)
{
    if (!SCM_PTRP(obj)) {
        if (SCM_TRUEP(obj) || SCM_FALSEP(obj)) return SCM_CLASS_BOOL;
        if (SCM_NULLP(obj)) return SCM_CLASS_NULL;
        if (SCM_CHARP(obj)) return SCM_CLASS_CHAR;
        if (SCM_INTP(obj))  return SCM_CLASS_INTEGER;
        else return SCM_CLASS_UNKNOWN;
    } else {
        return obj->klass;
    }
}

/*--------------------------------------------------------------
 * Metainformation accessors
 */
/* TODO: disable modification of system-builtin classes */

static ScmObj class_name(ScmClass *klass)
{
    return klass->name;
}

static void class_name_set(ScmClass *klass, ScmObj val)
{
    klass->name = val;
}

static ScmObj class_cpl(ScmClass *klass)
{
    return klass->cpl;
}

static void class_cpl_set(ScmClass *klass, ScmObj val)
{
    /* have to make sure things are consistent */
    int len, object_inherited = FALSE;
    ScmObj cp;
    ScmClass **p;

    if (!SCM_PAIRP(val)) goto err;
    if (SCM_CAR(val) != SCM_OBJ(klass)) goto err;
    /* set up the cpa */
    cp = SCM_CDR(val);
    if ((len = Scm_Length(cp)) < 0) goto err;
    klass->cpa = class_list_to_array(cp, len);
    if (klass->cpa[len-1] != SCM_CLASS_TOP) goto err;
    klass->cpl = Scm_CopyList(val);
    /* find correct allocation method */
    klass->allocate = NULL;
    for (p = klass->cpa; *p; p++) {
        if (SCM_CLASS_FINAL_P(*p))
            Scm_Error("you can't inherit a final class %S", *p);
        if ((*p)->allocate) {
            if ((*p)->allocate != object_allocate) {
                if (klass->allocate && klass->allocate != (*p)->allocate) {
                    Scm_Error("class precedence list has more than one C-defined base class (except <object>): %S", val);
                }
                klass->allocate = (*p)->allocate;
            } else {
                object_inherited = TRUE;
            }
        }
    }
    if (!object_inherited)
        Scm_Error("class precedence list doesn't have a base class: %S", val);
    if (!klass->allocate)
        klass->allocate = object_allocate; /* default */
    return;
  err:
    Scm_Error("class precedence list must be a proper list of class "
              "metaobject, begenning from the class itself owing the list, "
              "and ending by the class <top>: %S", val);
}

static ScmObj class_direct_supers(ScmClass *klass)
{
    return klass->directSupers;
}

static void class_direct_supers_set(ScmClass *klass, ScmObj val)
{
    ScmObj vp;
    SCM_FOR_EACH(vp, val) {
        if (!Scm_TypeP(SCM_CAR(vp), SCM_CLASS_CLASS))
            Scm_Error("non-class object found in direct superclass list: %S",
                      SCM_CAR(vp));
    }
    klass->directSupers = val;
}

static ScmObj class_direct_slots(ScmClass *klass)
{
    return klass->directSlots;
}

static void class_direct_slots_set(ScmClass *klass, ScmObj val)
{
    ScmObj vp;
    SCM_FOR_EACH(vp, val) {
        if (!SCM_PAIRP(SCM_CAR(vp)))
            Scm_Error("bad slot spec found in direct slot list: %S",
                      SCM_CAR(vp));
    }
    klass->directSlots = val;
}

static ScmObj class_slots_ref(ScmClass *klass)
{
    return klass->slots;
}

static void class_slots_set(ScmClass *klass, ScmObj val)
{
    ScmObj vp;
    SCM_FOR_EACH(vp, val) {
        if (!SCM_PAIRP(SCM_CAR(vp)))
            Scm_Error("bad slot spec found in slot list: %S",
                      SCM_CAR(vp));
    }
    klass->slots = val;
}

static ScmObj class_accessors(ScmClass *klass)
{
    return klass->accessors;
}

static void class_accessors_set(ScmClass *klass, ScmObj val)
{
    ScmObj vp;
    SCM_FOR_EACH(vp, val) {
        if (!SCM_PAIRP(SCM_CAR(vp))
            || !SCM_SLOT_ACCESSOR_P(SCM_CDAR(vp)))
            Scm_Error("slot accessor list must be an assoc-list of slot name and slot accessor object, but found: %S",
                      SCM_CAR(vp));
    }
    klass->accessors = val;
}

static ScmObj class_direct_subclasses(ScmClass *klass)
{
    return klass->directSubclasses;
}

static void class_direct_subclasses_set(ScmClass *klass, ScmObj val)
{
    /* TODO: check argument vailidity */
    klass->directSubclasses = val;
}

static ScmObj class_numislots(ScmClass *klass)
{
    return Scm_MakeInteger(klass->numInstanceSlots);
}

static void class_numislots_set(ScmClass *klass, ScmObj snf)
{
    int nf = 0;
    if (!SCM_INTP(snf) || (nf = SCM_INT_VALUE(snf)) < 0) {
        Scm_Error("invalid argument: %S", snf);
        /*NOTREACHED*/
    }
    klass->numInstanceSlots = nf;
}

/*--------------------------------------------------------------
 * External interface
 */

int Scm_SubtypeP(ScmClass *sub, ScmClass *type)
{
    ScmClass **p;
    if (sub == type) return TRUE;

    p = sub->cpa;
    while (*p) {
        if (*p++ == type) return TRUE;
    }
    return FALSE;
}

int Scm_TypeP(ScmObj obj, ScmClass *type)
{
    return Scm_SubtypeP(Scm_ClassOf(obj), type);
}

/*
 * compute-cpl
 */
static ScmObj compute_cpl_cb(ScmObj k, void *dummy)
{
    return SCM_CLASS(k)->directSupers;
}

ScmObj Scm_ComputeCPL(ScmClass *klass)
{
    ScmObj seqh = SCM_NIL, seqt, ds, dp, result;

    /* a trick to ensure we have <object> <top> at the end of CPL. */
    ds = Scm_Delete(SCM_OBJ(SCM_CLASS_OBJECT), klass->directSupers,
                    SCM_CMP_EQ);
    ds = Scm_Delete(SCM_OBJ(SCM_CLASS_TOP), ds, SCM_CMP_EQ);
    ds = Scm_Append2(ds, SCM_LIST1(SCM_OBJ(SCM_CLASS_OBJECT)));
    SCM_APPEND1(seqh, seqt, ds);

    SCM_FOR_EACH(dp, klass->directSupers) {
        if (!Scm_TypeP(SCM_CAR(dp), SCM_CLASS_CLASS))
            Scm_Error("non-class found in direct superclass list: %S",
                      klass->directSupers);
        if (SCM_CAR(dp) == SCM_OBJ(SCM_CLASS_OBJECT)
            || SCM_CAR(dp) == SCM_OBJ(SCM_CLASS_TOP))
            continue;
        SCM_APPEND1(seqh, seqt, SCM_CLASS(SCM_CAR(dp))->cpl);
    }
    SCM_APPEND1(seqh, seqt, Scm_ObjectClass.cpl);
    
    result = Scm_MonotonicMerge(SCM_OBJ(klass), seqh, compute_cpl_cb, NULL);
    if (SCM_FALSEP(result))
        Scm_Error("discrepancy found in class precedence lists of the superclasses: %S",
                  klass->directSupers);
    return result;
}

/*=====================================================================
 * Scheme slot access
 */

/* Scheme slots are simply stored in ScmObj array.  What complicates
 * the matter is that we allow any C structure to be a base class of
 * Scheme class, so there may be an offset for such slots.
 */
/* Unbound slot: if the slot value yields either SCM_UNBOUND or
 * SCM_UNDEFINED, a generic function slot-unbound is called.
 * We count SCM_UNDEFINED as unbound so that a Scheme program can
 * make slot unbound, especially needed for procedural slots.
 */

/* dummy structure to access Scheme slots */
typedef struct ScmInstanceRec {
    SCM_HEADER;
    ScmObj slots[1];
} ScmInstance;

#define SCM_INSTANCE(obj)      ((ScmInstance *)obj)

static inline int scheme_slot_index(ScmObj obj, int number)
{
    ScmClass *k = SCM_CLASS_OF(obj);
    int offset = k->instanceSlotOffset;
    if (offset == 0)
        Scm_Error("scheme slot accessor called with C-defined object %S.  implementation error?",
                  obj);
    if (number < 0 || number > k->numInstanceSlots)
        Scm_Error("instance slot index %d out of bounds for %S",
                  number, obj);
    return number-offset+1;
}


static inline ScmObj scheme_slot_ref(ScmObj obj, int number)
{
    int index = scheme_slot_index(obj, number);
    return SCM_INSTANCE(obj)->slots[index];
}

static inline void scheme_slot_set(ScmObj obj, int number, ScmObj val)
{
    int index = scheme_slot_index(obj, number);
    SCM_INSTANCE(obj)->slots[index] = val;
}

static void scheme_slot_default(ScmObj obj)
{
    int index = scheme_slot_index(obj, 0);
    int count = SCM_CLASS_OF(obj)->numInstanceSlots;
    int i;
    for (i=0; i<count; i++, index++)
        SCM_INSTANCE(obj)->slots[index] = SCM_UNBOUND;
}

/* initialize slot according to its accessor spec */
static ScmObj slot_initialize_cc(ScmObj result, void **data)
{
    ScmObj obj = data[0];
    ScmObj slot = data[1];
    return Scm_VMSlotSet(obj, slot, result);
}

static ScmObj slot_initialize(ScmObj obj, ScmObj acc, ScmObj initargs)
{
    ScmObj slot = SCM_CAR(acc);
    ScmSlotAccessor *ca = SCM_SLOT_ACCESSOR(SCM_CDR(acc));
    /* (1) see if we have init-keyword */
    if (SCM_KEYWORDP(ca->initKeyword)) {
        ScmObj v = Scm_GetKeyword(ca->initKeyword, initargs, SCM_UNDEFINED);
        if (!SCM_UNDEFINEDP(v)) return Scm_VMSlotSet(obj, slot, v);
        v = SCM_UNBOUND;
    }
    /* (2) use init-value or init-thunk.  this only applies to the      
       instance-allocated slot. */
    if (ca->slotNumber >= 0) {
        if (!SCM_UNBOUNDP(ca->initValue))
            return Scm_VMSlotSet(obj, slot, ca->initValue);
        if (SCM_PROCEDUREP(ca->initThunk)) {
            void *data[2];
            data[0] = (void*)obj;
            data[1] = (void*)slot;
            Scm_VMPushCC(slot_initialize_cc, data, 2);
            return Scm_VMApply(ca->initThunk, SCM_NIL);
        }
    }
    return SCM_UNDEFINED;
}

/*-------------------------------------------------------------------
 * slot-ref, slot-set! and families
 */
inline ScmSlotAccessor *Scm_GetSlotAccessor(ScmClass *klass, ScmObj slot)
{
    ScmObj p = Scm_Assq(slot, klass->accessors);
    if (!SCM_PAIRP(p)) return NULL;
    if (!SCM_XTYPEP(SCM_CDR(p), SCM_CLASS_SLOT_ACCESSOR))
        Scm_Error("slot accessor information of class %S, slot %S is screwed up.",
                  SCM_OBJ(klass), slot);
    return SCM_SLOT_ACCESSOR(SCM_CDR(p));
}

#define SLOT_UNBOUND(klass, obj, slot)                  \
    Scm_VMApply(SCM_OBJ(&Scm_GenericSlotUnbound),       \
                SCM_LIST3(SCM_OBJ(klass), obj, slot))

static ScmObj slot_ref_cc(ScmObj result, void **data)
{
    ScmObj obj = data[0];
    ScmObj slot = data[1];
    int boundp = (int)data[2];

    if (SCM_UNBOUNDP(result) || SCM_UNDEFINEDP(result)) {
        if (boundp)
            return SCM_FALSE;
        else 
            return SLOT_UNBOUND(Scm_ClassOf(obj), obj, slot);
    } else {
        if (boundp)
            return SCM_TRUE;
        else 
            return result;
    }
}

ScmObj Scm_VMSlotRef(ScmObj obj, ScmObj slot, int boundp)
{
    ScmClass *klass = Scm_ClassOf(obj);
    ScmSlotAccessor *ca = Scm_GetSlotAccessor(klass, slot);
    ScmObj val = SCM_UNBOUND;

    if (ca == NULL) {
        return Scm_VMApply(SCM_OBJ(&Scm_GenericSlotMissing),
                           SCM_LIST3(SCM_OBJ(klass), obj, slot));
    }
    if (ca->getter) {
        val = ca->getter(obj);
    } else if (ca->slotNumber >= 0) {
        val = scheme_slot_ref(obj, ca->slotNumber);
    } else if (SCM_PAIRP(ca->schemeAccessor)
               && SCM_PROCEDUREP(SCM_CAR(ca->schemeAccessor))) {
        void *data[3];
        data[0] = obj;
        data[1] = slot;
        data[2] = (void*)boundp;
        Scm_VMPushCC(slot_ref_cc, data, 3);
        return Scm_VMApply(SCM_CAR(ca->schemeAccessor), SCM_LIST1(obj));
    } else {
        Scm_Error("don't know how to retrieve value of slot %S of object %S (MOP error?)",
                  slot, obj);
    }
    if (boundp) {
        return SCM_MAKE_BOOL(!(SCM_UNBOUNDP(val) || SCM_UNDEFINEDP(val)));
    } else {
        if (SCM_UNBOUNDP(val) || SCM_UNDEFINEDP(val))
            return SLOT_UNBOUND(klass, obj, slot);
        else
            return val;
    }
}

ScmObj Scm_VMSlotSet(ScmObj obj, ScmObj slot, ScmObj val)
{
    ScmClass *klass = Scm_ClassOf(obj);
    ScmSlotAccessor *ca = Scm_GetSlotAccessor(klass, slot);
    
    if (ca == NULL) {
        return Scm_VMApply(SCM_OBJ(&Scm_GenericSlotMissing),
                           SCM_LIST4(SCM_OBJ(klass), obj, slot, val));
    }
    if (ca->setter) {
        ca->setter(obj, val);
    } else if (ca->slotNumber >= 0) {
        scheme_slot_set(obj, ca->slotNumber, val);
    } else if (SCM_PAIRP(ca->schemeAccessor)
               && SCM_PROCEDUREP(SCM_CDR(ca->schemeAccessor))) {
        return Scm_VMApply(SCM_CDR(ca->schemeAccessor), SCM_LIST2(obj, val));
    } else {
        Scm_Error("slot %S of class %S is read-only", slot, SCM_OBJ(klass));
    }
    return SCM_UNDEFINED;
}

/* slot-bound?
 *   (define (slot-bound? obj slot)
 *      (slot-bound-using-class (class-of obj) obj slot))
 */
ScmObj Scm_VMSlotBoundP(ScmObj obj, ScmObj slot)
{
    return Scm_VMApply(SCM_OBJ(&Scm_GenericSlotBoundUsingClassP),
                       SCM_LIST3(SCM_OBJ(Scm_ClassOf(obj)), obj, slot));
}

static ScmObj slot_bound_using_class_p(ScmNextMethod *nm, ScmObj *args,
                                       int nargs, void *data)
{
    return Scm_VMSlotRef(args[1], args[2], TRUE);
}

static ScmClass *slot_bound_using_class_p_SPEC[] = {
    SCM_CLASS_CLASS, SCM_CLASS_TOP, SCM_CLASS_TOP
};
static SCM_DEFINE_METHOD(slot_bound_using_class_p_rec,
                         &Scm_GenericSlotBoundUsingClassP,
                         3, 0,
                         slot_bound_using_class_p_SPEC,
                         slot_bound_using_class_p, NULL);

#if 0
/* NB: Scm_GetSlotRefProc(CLASS, SLOT) and Scm_GetSlotSetProc(CLASS, SLOT)
 *   return subrs that can be used in place of (slot-ref OBJ SLOT) and
 *   and (slot-set! OBJ SLOT), where OBJ's class is CLASS.   The intention
 *   is to pre-calculate slot lookup and type dispatch according to
 *   the slot implementation.  The rudimental benchmark showed it is
 *   faster than applying slot-ref/slot-set!.
 *
 *   However, I found that inlining slot-ref/slot-set! makes
 *   the code even faster, so you don't need to do all these precalculation
 *   for speed.   I leave the code inside #if 0 -- #endif, for in future
 *   I may find it useful...
 */
struct slot_acc_packet {
    union {
        ScmNativeGetterProc cgetter;
        ScmNativeSetterProc csetter;
        int slotNum;
        ScmObj sgetter;
        ScmObj ssetter;
    } method;
    ScmObj slot;
};

static ScmObj slot_ref_native(ScmObj *args, int nargs, void *data)
{
    struct slot_acc_packet *p = (struct slot_acc_packet *)data;
    ScmObj val = p->method.cgetter(args[0]);
    if (SCM_UNBOUNDP(val)) 
        return SLOT_UNBOUND(Scm_ClassOf(args[0]), args[0], p->slot);
    else
        return val;
}

static ScmObj slot_ref_instance(ScmObj *args, int nargs, void *data)
{
    struct slot_acc_packet *p = (struct slot_acc_packet *)data;
    ScmObj val = scheme_slot_ref(args[0], p->method.slotNum);
    if (SCM_UNBOUNDP(val)) 
        return SLOT_UNBOUND(Scm_ClassOf(args[0]), args[0], p->slot);
    else
        return val;
}

static ScmObj slot_ref_procedural(ScmObj *args, int nargs, void *data)
{
    struct slot_acc_packet *p = (struct slot_acc_packet *)data;
    void *next[2];
    next[0] = args[0];
    next[1] = p->slot;
    Scm_VMPushCC(slot_ref_cc, next, 2);
    return Scm_VMApply(p->method.sgetter, SCM_LIST1(args[0]));
}

static ScmObj slot_ref_missing(ScmObj *args, int nargs, void *data)
{
    struct slot_acc_packet *p = (struct slot_acc_packet *)data;
    return Scm_VMApply(SCM_OBJ(&Scm_GenericSlotMissing),
                       SCM_LIST3(SCM_OBJ(Scm_ClassOf(args[0])), args[0],
                                         p->slot));

}

static ScmObj slot_set_native(ScmObj *args, int nargs, void *data)
{
    struct slot_acc_packet *p = (struct slot_acc_packet *)data;
    p->method.csetter(args[0], args[1]);
    return SCM_UNDEFINED;
}

static ScmObj slot_set_instance(ScmObj *args, int nargs, void *data)
{
    struct slot_acc_packet *p = (struct slot_acc_packet *)data;
    scheme_slot_set(args[0], p->method.slotNum, args[1]);
    return SCM_UNDEFINED;
}

static ScmObj slot_set_procedural(ScmObj *args, int nargs, void *data)
{
    struct slot_acc_packet *p = (struct slot_acc_packet *)data;
    return Scm_VMApply(p->method.ssetter, SCM_LIST2(args[0], args[1]));
}

static ScmObj slot_set_missing(ScmObj *args, int nargs, void *data)
{
    struct slot_acc_packet *p = (struct slot_acc_packet *)data;
    return Scm_VMApply(SCM_OBJ(&Scm_GenericSlotMissing),
                       SCM_LIST4(SCM_OBJ(Scm_ClassOf(args[0])), args[0],
                                         p->slot, args[1]));

}

ScmObj Scm_GetSlotRefProc(ScmClass *klass, ScmObj slot)
{
    ScmSlotAccessor *ca = Scm_GetSlotAccessor(klass, slot);
    struct slot_acc_packet *p = SCM_NEW(struct slot_acc_packet);
    ScmObj outp = Scm_MakeOutputStringPort(), name;
    p->slot = slot;

    Scm_Printf(SCM_PORT(outp), "slot-ref %S %S", klass->name, slot);
    name = Scm_GetOutputString(SCM_PORT(outp));

    if (ca == NULL) {
        return Scm_MakeSubr(slot_ref_missing, p, 1, 0, name);
    }
    if (ca->getter) {
        p->method.cgetter = ca->getter;
        return Scm_MakeSubr(slot_ref_native, p, 1, 0, name);
    }
    if (ca->slotNumber >= 0) {
        p->method.slotNum = ca->slotNumber;
        return Scm_MakeSubr(slot_ref_instance, p, 1, 0, name);
    }
    if (SCM_PAIRP(ca->schemeAccessor)
        && SCM_PROCEDUREP(SCM_CAR(ca->schemeAccessor))) {
        p->method.sgetter = SCM_CAR(ca->schemeAccessor);
        return Scm_MakeSubr(slot_ref_procedural, p, 1, 0, name);
    } else {
        Scm_Error("don't know how to make slot referencer of slot %S of class %S (MOP error?)",
                  slot, klass);
        return SCM_UNDEFINED;
    }
}

ScmObj Scm_GetSlotSetProc(ScmClass *klass, ScmObj slot)
{
    ScmSlotAccessor *ca = Scm_GetSlotAccessor(klass, slot);
    struct slot_acc_packet *p = SCM_NEW(struct slot_acc_packet);
    ScmObj outp = Scm_MakeOutputStringPort(), name;
    p->slot = slot;

    Scm_Printf(SCM_PORT(outp), "slot-set %S %S", klass->name, slot);
    name = Scm_GetOutputString(SCM_PORT(outp));
    
    if (ca == NULL) {
        return Scm_MakeSubr(slot_set_missing, p, 2, 0, name);
    }
    if (ca->setter) {
        p->method.csetter = ca->setter;
        return Scm_MakeSubr(slot_set_native, p, 2, 0, name);
    }
    if (ca->slotNumber >= 0) {
        p->method.slotNum = ca->slotNumber;
        return Scm_MakeSubr(slot_set_instance, p, 2, 0, name);
    }
    if (SCM_PAIRP(ca->schemeAccessor)
        && SCM_PROCEDUREP(SCM_CDR(ca->schemeAccessor))) {
        p->method.ssetter = SCM_CDR(ca->schemeAccessor);
        return Scm_MakeSubr(slot_set_procedural, p, 2, 0, name);
    } else {
        Scm_Error("slot %S of class %S is read-only", slot, SCM_OBJ(klass));
        return SCM_UNDEFINED;
    }
}
#endif

/*
 * Builtin object initializer
 * This is the fallback method of generic initialize.  Since all the
 * Scheme-defined objects will be initialized by object_initialize,
 * this method is called only for built-in classes.
 */
static ScmObj builtin_initialize(ScmObj *args, int nargs, ScmGeneric *gf)
{
    ScmObj instance, initargs, ip, ap;
    ScmClass *klass;
    SCM_ASSERT(nargs == 2);
    instance = args[0];
    initargs = args[1];
    if (Scm_Length(initargs) % 2) {
        Scm_Error("initializer list is not even: %S", initargs);
    }
    klass = Scm_ClassOf(instance);
    SCM_FOR_EACH(ap, klass->accessors) {
        ScmSlotAccessor *acc = SCM_SLOT_ACCESSOR(SCM_CDAR(ap));
        if (acc->setter && SCM_KEYWORDP(acc->initKeyword)) {
            ScmObj val = Scm_GetKeyword(acc->initKeyword, initargs, SCM_UNDEFINED);
            if (!SCM_UNDEFINEDP(val)) {
                acc->setter(instance, val);
            }
        }
    }
    return instance;
}

/*--------------------------------------------------------------
 * Slot accessor object
 */

/* we initialize fields appropriately here. */
static ScmObj slot_accessor_allocate(ScmClass *klass, ScmObj initargs)
{
    ScmSlotAccessor *sa = SCM_NEW(ScmSlotAccessor);
    ScmObj slotnum, slotget, slotset;

    SCM_SET_CLASS(sa, klass);
    sa->getter = NULL;
    sa->setter = NULL;
    sa->initValue =   Scm_GetKeyword(key_init_value, initargs, SCM_UNDEFINED);
    if (sa->initValue == SCM_UNDEFINED) sa->initValue = SCM_UNBOUND;
    sa->initKeyword = Scm_GetKeyword(key_init_keyword, initargs, SCM_FALSE);
    sa->initThunk =   Scm_GetKeyword(key_init_thunk, initargs, SCM_FALSE);

    slotnum = Scm_GetKeyword(key_slot_num, initargs, SCM_FALSE);
    if (SCM_INTP(slotnum) && SCM_INT_VALUE(slotnum) >= 0) {
        sa->slotNumber = SCM_INT_VALUE(slotnum);
    } else {
        sa->slotNumber = -1;
    }
    slotget = Scm_GetKeyword(key_slot_ref, initargs, SCM_FALSE);
    slotset = Scm_GetKeyword(key_slot_set, initargs, SCM_FALSE);
    if (SCM_PROCEDUREP(slotget) && SCM_PROCEDUREP(slotset)) {
        sa->schemeAccessor = Scm_Cons(slotget, slotset);
    } else {
        sa->schemeAccessor = SCM_FALSE;
    }
    return SCM_OBJ(sa);
}

static void slot_accessor_print(ScmObj obj, ScmPort *out, ScmWriteContext *ctx)
{
    ScmSlotAccessor *sa = SCM_SLOT_ACCESSOR(obj);
    
    Scm_Printf(out, "#<slot-accessor ");
    if (sa->getter) Scm_Printf(out, "native");
    else if (SCM_PAIRP(sa->schemeAccessor)) Scm_Printf(out, "proc");
    else if (sa->slotNumber >= 0) Scm_Printf(out, "%d", sa->slotNumber);
    else Scm_Printf(out, "unknown");
    if (!SCM_FALSEP(sa->initKeyword))
        Scm_Printf(out, " %S", sa->initKeyword);
    Scm_Printf(out, ">");
}

/* some information is visible from Scheme world */
static ScmObj slot_accessor_init_value(ScmSlotAccessor *sa)
{
    return sa->initValue;
}

static ScmObj slot_accessor_init_keyword(ScmSlotAccessor *sa)
{
    return sa->initKeyword;
}

static ScmObj slot_accessor_init_thunk(ScmSlotAccessor *sa)
{
    return sa->initThunk;
}

static ScmObj slot_accessor_slot_number(ScmSlotAccessor *sa)
{
    return SCM_MAKE_INT(sa->slotNumber);
}

static void slot_accessor_slot_number_set(ScmSlotAccessor *sa, ScmObj val)
{
    int n = 0;
    if (!SCM_INTP(val) || ((n = SCM_INT_VALUE(val)) < 0))
        Scm_Error("small positive integer required, but got %S", val);
    sa->slotNumber = n;
}

static ScmObj slot_accessor_scheme_accessor(ScmSlotAccessor *sa)
{
    return sa->schemeAccessor;
}

static void slot_accessor_scheme_accessor_set(ScmSlotAccessor *sa, ScmObj p)
{
    /* TODO: check */
    sa->schemeAccessor = p;
}

/*=====================================================================
 * <object> class initialization
 */

static ScmObj object_allocate(ScmClass *klass, ScmObj initargs)
{
    int size = sizeof(ScmObj)*(klass->numInstanceSlots) + sizeof(ScmHeader);
    ScmObj obj = SCM_NEW2(ScmObj, size);
    SCM_SET_CLASS(obj, klass);
    scheme_slot_default(obj);
    return SCM_OBJ(obj);
}

/* (initialize <object> initargs) */
static ScmObj object_initialize_cc(ScmObj result, void **data)
{
    ScmObj obj = SCM_OBJ(data[0]);
    ScmObj accs = SCM_OBJ(data[1]);
    ScmObj initargs = SCM_OBJ(data[2]);
    void *next[3];
    if (SCM_NULLP(accs)) return obj;
    next[0] = obj;
    next[1] = SCM_CDR(accs);
    next[2] = initargs;
    Scm_VMPushCC(object_initialize_cc, next, 3);
    return slot_initialize(obj, SCM_CAR(accs), initargs);
}

static ScmObj object_initialize(ScmNextMethod *nm, ScmObj *args, int nargs,
                                void *data)
{
    ScmObj obj = args[0];
    ScmObj initargs = args[1];
    ScmObj accs = Scm_ClassOf(obj)->accessors;
    void *next[3];
    if (SCM_NULLP(accs)) return obj;
    next[0] = obj;
    next[1] = SCM_CDR(accs);
    next[2] = initargs;
    Scm_VMPushCC(object_initialize_cc, next, 3);
    return slot_initialize(obj, SCM_CAR(accs), initargs);
}

static ScmClass *object_initialize_SPEC[] = {
    SCM_CLASS_OBJECT, SCM_CLASS_LIST
};
static SCM_DEFINE_METHOD(object_initialize_rec,
                         &Scm_GenericInitialize,
                         2, 0,
                         object_initialize_SPEC,
                         object_initialize, NULL);

/*=====================================================================
 * Generic function
 */

static ScmObj generic_allocate(ScmClass *klass, ScmObj initargs)
{
    ScmGeneric *instance;
    int nslots = klass->numInstanceSlots;
    instance = SCM_NEW2(ScmGeneric*,
                        sizeof(ScmGeneric) + sizeof(ScmObj)*nslots);
    SCM_SET_CLASS(instance, klass);
    SCM_PROCEDURE_INIT(instance, 0, 0, SCM_PROC_GENERIC, SCM_FALSE);
    instance->methods = SCM_NIL;
    instance->fallback = Scm_NoNextMethod;
    instance->data = NULL;
    scheme_slot_default(SCM_OBJ(instance));
    return SCM_OBJ(instance);
}

static void generic_print(ScmObj obj, ScmPort *port, ScmWriteContext *ctx)
{
    Scm_Printf(port, "#<generic %S (%d)>",
               SCM_GENERIC(obj)->common.info,
               Scm_Length(SCM_GENERIC(obj)->methods));
}

/*
 * (initialize <generic> &key name)  - default initialize function for gf
 */
static ScmObj generic_initialize(ScmNextMethod *nm, ScmObj *args, int nargs,
                                 void *data)
{
    ScmGeneric *g = SCM_GENERIC(args[0]);
    ScmObj initargs = args[1], name;
    name = Scm_GetKeyword(key_name, initargs, SCM_FALSE);
    g->common.info = name;
    return SCM_OBJ(g);
}

static ScmClass *generic_initialize_SPEC[] = {
    SCM_CLASS_GENERIC, SCM_CLASS_LIST
};
static SCM_DEFINE_METHOD(generic_initialize_rec,
                         &Scm_GenericInitialize,
                         2, 0,
                         generic_initialize_SPEC,
                         generic_initialize, NULL);

/*
 * Accessors
 */
static ScmObj generic_name(ScmGeneric *gf)
{
    return gf->common.info;
}

static void generic_name_set(ScmGeneric *gf, ScmObj val)
{
    gf->common.info = val;
}

static ScmObj generic_methods(ScmGeneric *gf)
{
    return gf->methods;
}

static void generic_methods_set(ScmGeneric *gf, ScmObj val)
{
    gf->methods = val;
}

/* Make base generic function from C */
ScmObj Scm_MakeBaseGeneric(ScmObj name,
                           ScmObj (*fallback)(ScmObj *, int, ScmGeneric*),
                           void *data)
{
    ScmGeneric *gf = SCM_GENERIC(generic_allocate(SCM_CLASS_GENERIC, SCM_NIL));
    gf->common.info = name;
    if (fallback) {
        gf->fallback = fallback;
        gf->data = data;
    }
    return SCM_OBJ(gf);
}

/* default "default method" */
ScmObj Scm_NoNextMethod(ScmObj *args, int nargs, ScmGeneric *gf)
{
    Scm_Error("no applicable method for %S with arguments %S",
              SCM_OBJ(gf), Scm_ArrayToList(args, nargs));
    return SCM_UNDEFINED;       /* dummy */
}

/* another handy "default method", which does nothing. */
ScmObj Scm_NoOperation(ScmObj *arg, int nargs, ScmGeneric *gf)
{
    return SCM_UNDEFINED;
}

/* compute-applicable-methods */
ScmObj Scm_ComputeApplicableMethods(ScmGeneric *gf, ScmObj *args, int nargs)
{
    ScmObj methods = gf->methods, mp;
    ScmObj h = SCM_NIL, t = SCM_NIL;

    SCM_FOR_EACH(mp, methods) {
        ScmMethod *m = SCM_METHOD(SCM_CAR(mp));
        ScmObj *ap;
        ScmClass **sp;
        int n;
        
        if (nargs < m->common.required) continue;
        if (!m->common.optional && nargs > m->common.required) continue;
        for (ap = args, sp = m->specializers, n = 0;
             n < m->common.required;
             ap++, sp++, n++) {
            if (!Scm_SubtypeP(Scm_ClassOf(*ap), *sp)) break;
        }
        if (n == m->common.required) SCM_APPEND1(h, t, SCM_OBJ(m));
    }
    return h;
}

static ScmObj compute_applicable_methods(ScmNextMethod *nm,
                                         ScmObj *args,
                                         int nargs,
                                         void *data)
{
    ScmGeneric *gf = SCM_GENERIC(args[0]);
    ScmObj arglist = args[1], ap;
    ScmObj *argv;
    int n = Scm_Length(arglist), i;
    if (n < 0) Scm_Error("bad argument list: %S", arglist);

    argv = SCM_NEW2(ScmObj *, sizeof(ScmObj)*n);
    i = 0;
    SCM_FOR_EACH(ap, arglist) argv[i++] = SCM_CAR(ap);
    return Scm_ComputeApplicableMethods(gf, argv, n);
}

static ScmClass *compute_applicable_methods_SPEC[] = {
    SCM_CLASS_GENERIC, SCM_CLASS_LIST
};
static SCM_DEFINE_METHOD(compute_applicable_methods_rec,
                         &Scm_GenericComputeApplicableMethods,
                         2, 0,
                         compute_applicable_methods_SPEC,
                         compute_applicable_methods, NULL);

/* method-more-specific? */
static inline int method_more_specific(ScmMethod *x, ScmMethod *y,
                                       ScmClass **targs, int nargs)
{
    ScmClass **xs = x->specializers;
    ScmClass **ys = y->specializers;
    ScmClass *ac, **acpl;
    int i;
    SCM_ASSERT(SCM_PROCEDURE_REQUIRED(x) == SCM_PROCEDURE_REQUIRED(y));
    for (i=0; i < SCM_PROCEDURE_REQUIRED(x); i++) {
        if (xs[i] != ys[i]) {
            ac = targs[i];
            if (xs[i] == ac) return TRUE;
            if (ys[i] == ac) return FALSE;
            for (acpl = ac->cpa; *acpl; acpl++) {
                if (xs[i] == *acpl) return TRUE;
                if (ys[i] == *acpl) return FALSE;
            }
            Scm_Panic("internal error: couldn't determine more specific method.");
        }
    }
    /* all specializers match.  the one without optional arg is more special.*/
    if (SCM_PROCEDURE_OPTIONAL(x)) return TRUE;
    else return FALSE;
}

static ScmObj method_more_specific_p(ScmNextMethod *nm, ScmObj *args,
                                     int nargs, void *data)
{
    ScmMethod *x = SCM_METHOD(args[0]);
    ScmMethod *y = SCM_METHOD(args[1]);
    ScmObj targlist = args[2], tp;
    ScmClass **targs;
    int ntargs = Scm_Length(targlist), i;
    if (ntargs < 0) Scm_Error("bad targ list: %S", targlist);
    targs = SCM_NEW2(ScmClass**, sizeof(ScmObj)*ntargs);
    i = 0;
    SCM_FOR_EACH(tp, targlist) {
        if (!Scm_TypeP(SCM_CAR(tp), SCM_CLASS_CLASS))
            Scm_Error("bad class object in type list: %S", SCM_CAR(tp));
        targs[i++] = SCM_CLASS(SCM_CAR(tp));
    }
    return SCM_MAKE_BOOL(method_more_specific(x, y, targs, ntargs));
}
static ScmClass *method_more_specific_p_SPEC[] = {
    SCM_CLASS_METHOD, SCM_CLASS_METHOD, SCM_CLASS_LIST
};
static SCM_DEFINE_METHOD(method_more_specific_p_rec,
                         &Scm_GenericMethodMoreSpecificP,
                         3, 0,
                         method_more_specific_p_SPEC,
                         method_more_specific_p, NULL);

/* sort-methods
 *  This is a naive implementation just to make things work.
 * TODO: can't we carry around the method list in array
 * instead of list, at least internally?
 */
#define STATIC_SORT_ARRAY_SIZE  32

ScmObj Scm_SortMethods(ScmObj methods, ScmObj *args, int nargs)
{
    ScmObj starray[STATIC_SORT_ARRAY_SIZE], *array = starray;
    ScmClass *sttargs[STATIC_SORT_ARRAY_SIZE], **targs = sttargs;
    int cnt = 0, len = Scm_Length(methods), step, i, j;
    ScmObj mp;

    if (len >= STATIC_SORT_ARRAY_SIZE)
        array = SCM_NEW2(ScmObj*, sizeof(ScmObj)*len);
    if (nargs >= STATIC_SORT_ARRAY_SIZE)
        targs = SCM_NEW2(ScmClass**, sizeof(ScmObj)*nargs);

    SCM_FOR_EACH(mp, methods) {
        if (!Scm_TypeP(SCM_CAR(mp), SCM_CLASS_METHOD))
            Scm_Error("bad method in applicable method list: %S", SCM_CAR(mp));
        array[cnt] = SCM_CAR(mp);
        cnt++;
    }
    for (i=0; i<nargs; i++) targs[i] = Scm_ClassOf(args[i]);

    for (step = len/2; step > 0; step /= 2) {
        for (i=step; i<len; i++) {
            for (j=i-step; j >= 0; j -= step) {
                if (method_more_specific(SCM_METHOD(array[j]),
                                         SCM_METHOD(array[j+step]),
                                         targs, nargs)) {
                    break;
                } else {
                    ScmObj tmp = array[j+step];
                    array[j+step] = array[j];
                    array[j] = tmp;
                }
            }
        }
    }
    return Scm_ArrayToList(array, len);
}

/*=====================================================================
 * Method
 */

static ScmObj method_allocate(ScmClass *klass, ScmObj initargs)
{
    ScmMethod *instance;
    int nslots = klass->numInstanceSlots;
    instance = SCM_NEW2(ScmMethod*,
                        sizeof(ScmMethod) + sizeof(ScmObj)*nslots);
    SCM_SET_CLASS(instance, klass);
    SCM_PROCEDURE_INIT(instance, 0, 0, SCM_PROC_METHOD, SCM_FALSE);
    instance->generic = NULL;
    instance->specializers = NULL;
    instance->func = NULL;
    scheme_slot_default(SCM_OBJ(instance));
    return SCM_OBJ(instance);
}

static void method_print(ScmObj obj, ScmPort *port, ScmWriteContext *ctx)
{
    Scm_Printf(port, "#<method %S>", SCM_METHOD(obj)->common.info);
}

/*
 * (initialize <method> (&key lamdba-list generic specializers body))
 *    Method initialization.   This needs to be hardcoded, since
 *    we can't call Scheme verison of initialize to initialize the
 *    "initialize" method (chicken-and-egg circularity).
 */
static ScmObj method_initialize(ScmNextMethod *nm, ScmObj *args, int nargs,
                                void *data)
{
    ScmMethod *m = SCM_METHOD(args[0]);
    ScmGeneric *g;
    ScmObj initargs = args[1];
    ScmObj llist = Scm_GetKeyword(key_lambda_list, initargs, SCM_FALSE);
    ScmObj generic = Scm_GetKeyword(key_generic, initargs, SCM_FALSE);
    ScmObj specs = Scm_GetKeyword(key_specializers, initargs, SCM_FALSE);
    ScmObj body = Scm_GetKeyword(key_body, initargs, SCM_FALSE);
    ScmClass **specarray;
    ScmObj lp;
    int speclen = 0, req = 0, opt = 0;

    if (!Scm_TypeP(generic, SCM_CLASS_GENERIC))
        Scm_Error("generic function required for :generic argument: %S",
                  generic);
    g = SCM_GENERIC(generic);
    if (!SCM_CLOSUREP(body))
        Scm_Error("closure required for :body argument: %S", body);
    if (!SCM_PAIRP(specs) ||(speclen = Scm_Length(specs)) < 0)
        Scm_Error("invalid specializers list: %S", specs);
    specarray = class_list_to_array(specs, speclen);

    /* find out # of args from lambda list */
    SCM_FOR_EACH(lp, llist) req++;
    if (!SCM_NULLP(lp)) opt++;

    if (SCM_PROCEDURE_REQUIRED(body) != req + opt + 1)
        Scm_Error("body doesn't match with lambda list: %S", body);
    if (speclen != req)
        Scm_Error("specializer list doesn't match with lambda list: %S",specs);
    
    m->common.required = req;
    m->common.optional = opt;
    m->common.info = Scm_Cons(g->common.info,
                              class_array_to_names(specarray, speclen));
    m->generic = g;
    m->specializers = specarray;
    m->func = NULL;
    m->data = SCM_CLOSURE(body)->code;
    m->env = SCM_CLOSURE(body)->env;
    return SCM_OBJ(m);
}

static ScmClass *method_initialize_SPEC[] = {
    SCM_CLASS_METHOD, SCM_CLASS_LIST
};
static SCM_DEFINE_METHOD(method_initialize_rec,
                         &Scm_GenericInitialize,
                         2, 0,
                         method_initialize_SPEC,
                         method_initialize, NULL);

/*
 * Accessors
 */
static ScmObj method_generic(ScmMethod *m)
{
    return m->generic ? SCM_OBJ(m->generic) : SCM_FALSE;
}

static void method_generic_set(ScmMethod *m, ScmObj val)
{
    if (SCM_GENERICP(val))
        m->generic = SCM_GENERIC(val);
    else
        Scm_Error("generic function required, but got %S", val);
}

static ScmObj method_specializers(ScmMethod *m)
{
    if (m->specializers) {
        return class_array_to_list(m->specializers, m->common.required);
    } else {
        return SCM_NIL;
    }
}

static void method_specializers_set(ScmMethod *m, ScmObj val)
{
    int len = Scm_Length(val);
    if (len != m->common.required)
        Scm_Error("specializer list doesn't match body's lambda list:", val);
    if (len == 0) 
        m->specializers = NULL;
    else 
        m->specializers = class_list_to_array(val, len);
}

/*
 * ADD-METHOD, and it's default method version.
 */
ScmObj Scm_AddMethod(ScmGeneric *gf, ScmMethod *method)
{
    ScmObj mp;
    
    if (method->generic && method->generic != gf)
        Scm_Error("method %S already added to a generic function %S",
                  method, method->generic);
    if (!SCM_FALSEP(Scm_Memq(SCM_OBJ(method), gf->methods)))
        Scm_Error("method %S already appears in a method list of generic %S"
                  " something wrong in MOP implementation?",
                  method, gf);
    method->generic = gf;
    /* Check if a method with the same signature exists */
    SCM_FOR_EACH(mp, gf->methods) {
        ScmMethod *mm = SCM_METHOD(SCM_CAR(mp));
        if (SCM_PROCEDURE_REQUIRED(method) == SCM_PROCEDURE_REQUIRED(mm)
            && SCM_PROCEDURE_OPTIONAL(method) == SCM_PROCEDURE_OPTIONAL(mm)) {
            ScmClass **sp1 = method->specializers;
            ScmClass **sp2 = mm->specializers;
            int i;
            for (i=0; i<SCM_PROCEDURE_REQUIRED(method); i++) {
                if (sp1[i] != sp2[i]) break;
            }
            if (i == SCM_PROCEDURE_REQUIRED(method)) {
                /* TODO: alert for MT */
                SCM_SET_CAR(mp, SCM_OBJ(method));
                return SCM_UNDEFINED;
            }
        }
    }
    gf->methods = Scm_Cons(SCM_OBJ(method), gf->methods);
    return SCM_UNDEFINED;
}

static ScmObj generic_addmethod(ScmNextMethod *nm, ScmObj *args, int nargs,
                                void *data)
{
    return Scm_AddMethod(SCM_GENERIC(args[0]), SCM_METHOD(args[1]));
}

static ScmClass *generic_addmethod_SPEC[] = {
    SCM_CLASS_GENERIC, SCM_CLASS_METHOD
};
static SCM_DEFINE_METHOD(generic_addmethod_rec, &Scm_GenericAddMethod, 2, 0,
                         generic_addmethod_SPEC, generic_addmethod, NULL);

/*=====================================================================
 * Next Method
 */

ScmObj Scm_MakeNextMethod(ScmGeneric *gf, ScmObj methods,
                          ScmObj *args, int nargs, int copyArgs)
{
    ScmNextMethod *nm = SCM_NEW(ScmNextMethod);
    SCM_SET_CLASS(nm, SCM_CLASS_NEXT_METHOD);
    SCM_PROCEDURE_INIT(nm, 0, 0, SCM_PROC_NEXT_METHOD, SCM_FALSE);
    nm->generic = gf;
    nm->methods = methods;
    if (copyArgs) {
        nm->args = SCM_NEW2(ScmObj*, sizeof(ScmObj)*nargs);
        memcpy(nm->args, args, sizeof(ScmObj)*nargs);
    } else {
        nm->args = args;
    }
    nm->nargs = nargs;
    return SCM_OBJ(nm);
}

static void next_method_print(ScmObj obj, ScmPort *out, ScmWriteContext *ctx)
{
    ScmNextMethod *nm = SCM_NEXT_METHOD(obj);
    ScmObj args = Scm_ArrayToList(nm->args, nm->nargs);
    Scm_Printf(out, "#<next-method %S %S>", nm->methods, args);
}

/*=====================================================================
 * Class initialization
 */

/* TODO: need a cleaner way! */
/* static declaration of some structures */

static ScmClassStaticSlotSpec class_slots[] = {
    SCM_CLASS_SLOT_SPEC("name", class_name, class_name_set),
    SCM_CLASS_SLOT_SPEC("cpl",  class_cpl, class_cpl_set),
    SCM_CLASS_SLOT_SPEC("direct-supers",  class_direct_supers, class_direct_supers_set),
    SCM_CLASS_SLOT_SPEC("accessors", class_accessors, class_accessors_set),
    SCM_CLASS_SLOT_SPEC("slots", class_slots_ref, class_slots_set),
    SCM_CLASS_SLOT_SPEC("direct-slots", class_direct_slots, class_direct_slots_set),
    SCM_CLASS_SLOT_SPEC("direct-subclasses", class_direct_subclasses, class_direct_subclasses_set),
    SCM_CLASS_SLOT_SPEC("num-instance-slots", class_numislots, class_numislots_set),
    { NULL }
};

static ScmClassStaticSlotSpec generic_slots[] = {
    SCM_CLASS_SLOT_SPEC("name", generic_name, generic_name_set),
    SCM_CLASS_SLOT_SPEC("methods", generic_methods, generic_methods_set),
    { NULL }
};

static ScmClassStaticSlotSpec method_slots[] = {
    SCM_CLASS_SLOT_SPEC("generic", method_generic, method_generic_set),
    SCM_CLASS_SLOT_SPEC("specializers", method_specializers, method_specializers_set),
    { NULL }
};

static ScmClassStaticSlotSpec slot_accessor_slots[] = {
    SCM_CLASS_SLOT_SPEC("init-value", slot_accessor_init_value, NULL),
    SCM_CLASS_SLOT_SPEC("init-keyword", slot_accessor_init_keyword, NULL),
    SCM_CLASS_SLOT_SPEC("init-thunk", slot_accessor_init_thunk, NULL),
    SCM_CLASS_SLOT_SPEC("slot-number", slot_accessor_slot_number, slot_accessor_slot_number_set),
    SCM_CLASS_SLOT_SPEC("getter-n-setter", slot_accessor_scheme_accessor, slot_accessor_scheme_accessor_set),
    { NULL }
};

/* booting class metaobject */
void bootstrap_class(ScmClass *k,
                     ScmClassStaticSlotSpec *specs)
{
    ScmObj slots = SCM_NIL, t = SCM_NIL;
    ScmObj acc = SCM_NIL;

    if (specs) {
        for (;specs->name; specs++) {
            ScmObj snam = SCM_INTERN(specs->name);
            acc = Scm_Acons(snam, SCM_OBJ(&specs->accessor), acc);
            specs->accessor.initKeyword = SCM_MAKE_KEYWORD(specs->name);
            SCM_APPEND1(slots, t,
                        Scm_List(snam,
                                 key_allocation, key_builtin,
                                 key_slot_accessor, SCM_OBJ(&specs->accessor),
                                 NULL));
        }
    }
    k->accessors = acc;
    k->directSlots = k->slots = slots;
}

void Scm_InitBuiltinClass(ScmClass *klass, const char *name,
                          ScmClassStaticSlotSpec *slots, ScmModule *mod)
{
    ScmObj h = SCM_NIL, t;
    ScmObj s = SCM_INTERN(name);
    ScmClass **p;
    
    klass->name = s;
    /* cpa -> cpl */
    SCM_APPEND1(h, t, SCM_OBJ(klass));
    for (p = klass->cpa; *p; p++) SCM_APPEND1(h, t, SCM_OBJ(*p));
    klass->cpl = h;
    if (SCM_PAIRP(SCM_CDR(h))) {
        klass->directSupers = SCM_LIST1(SCM_CADR(h));
    } else {
        klass->directSupers = SCM_NIL;
    }
    Scm_Define(mod, SCM_SYMBOL(s), SCM_OBJ(klass));
    if (slots) {
        bootstrap_class(klass, slots);
    }
}

void Scm_InitBuiltinGeneric(ScmGeneric *gf, const char *name, ScmModule *mod)
{
    ScmObj s = SCM_INTERN(name);
    gf->common.info = s;
    Scm_Define(mod, SCM_SYMBOL(s), SCM_OBJ(gf));
}

void Scm_InitBuiltinMethod(ScmMethod *m)
{
    m->common.info = Scm_Cons(m->generic->common.info,
                              class_array_to_names(m->specializers,
                                                   m->common.required));
    Scm_AddMethod(m->generic, m);
}

void Scm__InitClass(void)
{
    ScmModule *mod = Scm_GaucheModule();
    static ScmClass *nullcpa[] = { NULL }; /* for <top> */

    key_allocation = SCM_MAKE_KEYWORD("allocation");
    key_instance = SCM_MAKE_KEYWORD("instance");
    key_builtin = SCM_MAKE_KEYWORD("builtin");
    key_accessor = SCM_MAKE_KEYWORD("accessor");
    key_slot_accessor = SCM_MAKE_KEYWORD("slot-accessor");
    key_name = SCM_MAKE_KEYWORD("name");
    key_supers = SCM_MAKE_KEYWORD("supers");
    key_slots = SCM_MAKE_KEYWORD("slots");
    key_metaclass = SCM_MAKE_KEYWORD("metaclass");
    key_lambda_list = SCM_MAKE_KEYWORD("lambda-list");
    key_generic = SCM_MAKE_KEYWORD("generic");
    key_specializers = SCM_MAKE_KEYWORD("specializers");
    key_body = SCM_MAKE_KEYWORD("body");
    key_init_keyword = SCM_MAKE_KEYWORD("init-keyword");
    key_init_thunk = SCM_MAKE_KEYWORD("init-thunk");
    key_init_value = SCM_MAKE_KEYWORD("init-value");
    key_slot_num = SCM_MAKE_KEYWORD("slot-number");
    key_slot_ref = SCM_MAKE_KEYWORD("slot-ref");
    key_slot_set = SCM_MAKE_KEYWORD("slot-set!");

    /* booting class metaobject */
    Scm_TopClass.cpa = nullcpa;

#define CINIT(cl, nam) \
    Scm_InitBuiltinClass(cl, nam, NULL, mod)
    
    /* class.c */
    CINIT(SCM_CLASS_TOP,              "<top>");
    CINIT(SCM_CLASS_BOOL,             "<boolean>");
    CINIT(SCM_CLASS_CHAR,             "<char>");
    CINIT(SCM_CLASS_UNKNOWN,          "<unknown>");
    CINIT(SCM_CLASS_OBJECT,           "<object>");
    CINIT(SCM_CLASS_CLASS,            "<class>");
    bootstrap_class(&Scm_ClassClass, class_slots);
    CINIT(SCM_CLASS_GENERIC,          "<generic>");
    bootstrap_class(&Scm_GenericClass, generic_slots);
    Scm_GenericClass.flags |= SCM_CLASS_APPLICABLE;
    CINIT(SCM_CLASS_METHOD,           "<method>");
    bootstrap_class(&Scm_MethodClass, method_slots);
    Scm_MethodClass.flags |= SCM_CLASS_APPLICABLE;
    CINIT(SCM_CLASS_NEXT_METHOD,      "<next-method>");
    Scm_NextMethodClass.flags |= SCM_CLASS_APPLICABLE;
    CINIT(SCM_CLASS_SLOT_ACCESSOR,    "<slot-accessor>");
    bootstrap_class(&Scm_SlotAccessorClass, slot_accessor_slots);
    CINIT(SCM_CLASS_COLLECTION,       "<collection>");
    CINIT(SCM_CLASS_SEQUENCE,         "<sequence>");

    /* char.c */
    CINIT(SCM_CLASS_CHARSET,          "<char-set>");

    /* compile.c */
    CINIT(SCM_CLASS_IDENTIFIER,       "<identifier>");

    /* error.c */
    /* initialized in Scm__InitExceptions */

    /* hash.c */
    CINIT(SCM_CLASS_HASHTABLE,        "<hash-table>");

    /* keyword.c */
    CINIT(SCM_CLASS_KEYWORD,          "<keyword>");

    /* list.c */
    CINIT(SCM_CLASS_LIST,             "<list>");
    CINIT(SCM_CLASS_PAIR,             "<pair>");
    CINIT(SCM_CLASS_NULL,             "<null>");

    /* macro.c */
    CINIT(SCM_CLASS_SYNTAX,           "<syntax>");
    CINIT(SCM_CLASS_SYNTAX_PATTERN,   "<syntax-pattern>");
    CINIT(SCM_CLASS_SYNTAX_RULES,     "<syntax-rules>");

    /* module.c */
    CINIT(SCM_CLASS_MODULE,           "<module>");

    /* number.c */
    CINIT(SCM_CLASS_NUMBER,           "<number>");
    CINIT(SCM_CLASS_COMPLEX,          "<complex>");
    CINIT(SCM_CLASS_REAL,             "<real>");
    CINIT(SCM_CLASS_INTEGER,          "<integer>");

    /* port.c */
    CINIT(SCM_CLASS_PORT,             "<port>");

    /* proc.c */
    CINIT(SCM_CLASS_PROCEDURE,        "<procedure>");

    /* promise.c */
    CINIT(SCM_CLASS_PROMISE,          "<promise>");

    /* regexp.c */
    CINIT(SCM_CLASS_REGEXP,           "<regexp>");
    CINIT(SCM_CLASS_REGMATCH,         "<regmatch>");

    /* string.c */
    CINIT(SCM_CLASS_STRING,           "<string>");
    CINIT(SCM_CLASS_STRING_POINTER,   "<string-pointer>");

    /* symbol.c */
    CINIT(SCM_CLASS_SYMBOL,           "<symbol>");
    CINIT(SCM_CLASS_GLOC,             "<gloc>");

    /* system.c */
    /* initialized in Scm__InitSystem */
    
    /* vector.c */
    CINIT(SCM_CLASS_VECTOR,           "<vector>");
    
    /* vm.c */
    CINIT(SCM_CLASS_VM,               "<vm>");

#define GINIT(gf, nam) \
    Scm_InitBuiltinGeneric(gf, nam, mod);

    GINIT(&Scm_GenericMake, "make");
    GINIT(&Scm_GenericAllocate, "allocate-instance");
    GINIT(&Scm_GenericInitialize, "initialize");
    GINIT(&Scm_GenericAddMethod, "add-method!");
    GINIT(&Scm_GenericComputeCPL, "compute-cpl");
    GINIT(&Scm_GenericComputeSlots, "compute-slots");
    GINIT(&Scm_GenericComputeGetNSet, "compute-get-n-set");
    GINIT(&Scm_GenericComputeApplicableMethods, "compute-applicable-methods");
    GINIT(&Scm_GenericMethodMoreSpecificP, "method-more-specific?");
    GINIT(&Scm_GenericApplyGeneric, "apply-generic");
    GINIT(&Scm_GenericSlotMissing, "slot-missing");
    GINIT(&Scm_GenericSlotUnbound, "slot-unbound");
    GINIT(&Scm_GenericSlotBoundUsingClassP, "slot-bound-using-class?");

    Scm_InitBuiltinMethod(&class_allocate_rec);
    Scm_InitBuiltinMethod(&class_compute_cpl_rec);
    Scm_InitBuiltinMethod(&slot_bound_using_class_p_rec);
    Scm_InitBuiltinMethod(&object_initialize_rec);
    Scm_InitBuiltinMethod(&generic_initialize_rec);
    Scm_InitBuiltinMethod(&generic_addmethod_rec);
    Scm_InitBuiltinMethod(&method_initialize_rec);
    Scm_InitBuiltinMethod(&compute_applicable_methods_rec);
    Scm_InitBuiltinMethod(&method_more_specific_p_rec);
}
