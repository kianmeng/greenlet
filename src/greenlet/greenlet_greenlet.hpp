#ifndef GREENLET_GREENLET_HPP
#define GREENLET_GREENLET_HPP
/*
 * Declarations of the core data structures.
*/

#include <Python.h>
//#include "greenlet_internal.hpp"
#include "greenlet_compiler_compat.hpp"
#include "greenlet_refs.hpp"
#include "greenlet_cpython_compat.hpp"
#include "greenlet_allocator.hpp"

using greenlet::refs::OwnedObject;
using greenlet::refs::OwnedGreenlet;
using greenlet::refs::OwnedMainGreenlet;
using greenlet::refs::BorrowedGreenlet;

namespace greenlet
{
    class ExceptionState
    {
    private:
        G_NO_COPIES_OF_CLS(ExceptionState);

#if PY_VERSION_HEX >= 0x030700A3
        // Even though these are borrowed objects, we actually own
        // them, when they're not null.
        // XXX: Express that in the API.
    private:
        _PyErr_StackItem* exc_info;
        _PyErr_StackItem exc_state;
#else
        OwnedObject exc_type;
        OwnedObject exc_value;
        OwnedObject exc_traceback;
#endif
    public:
        ExceptionState();
        void operator<<(const PyThreadState *const tstate) G_NOEXCEPT;
        void operator>>(PyThreadState* tstate) G_NOEXCEPT;
        void clear() G_NOEXCEPT;

        int tp_traverse(visitproc visit, void* arg) G_NOEXCEPT;
        void tp_clear() G_NOEXCEPT;
    };

    template<typename T>
    void operator<<(const PyThreadState *const tstate, T& exc);

    template<typename IsPy37>
    class PythonStateContext
    {};

    template<>
    class PythonStateContext<GREENLET_WHEN_PY37>
    {
    protected:
        greenlet::refs::OwnedContext _context;
    public:
        inline const greenlet::refs::OwnedContext& context() const
        {
            return this->_context;
        }
        inline greenlet::refs::OwnedContext& context()
        {
            return this->_context;
        }

        inline void tp_clear()
        {
            this->_context.CLEAR();
        }

        template<typename T>
        inline static PyObject* context(T* tstate)
        {
            return tstate->context;
        }

        template<typename T>
        inline static void context(T* tstate, PyObject* new_context)
        {
            tstate->context = new_context;
            tstate->context_ver++;
        }
    };


    template<>
    class PythonStateContext<GREENLET_WHEN_NOT_PY37>
    {
    public:
        inline const greenlet::refs::OwnedContext& context() const
        {
            throw AttributeError("no context");
        }

        inline greenlet::refs::OwnedContext& context()
        {
            throw AttributeError("no context");
        }

        inline void tp_clear(){};

        template<typename T>
        inline static PyObject* context(T* UNUSED(tstate))
        {
            throw PyFatalError("This should never be called.");
        }

        template<typename T>
        inline static void context(T* UNUSED(tstate), PyObject* UNUSED(new_context))
        {
            throw PyFatalError("This should never be called.");
        }
    };

    class PythonState : public PythonStateContext<G_IS_PY37>
    {
    public:
        typedef greenlet::refs::OwnedReference<struct _frame> OwnedFrame;
    private:
        G_NO_COPIES_OF_CLS(PythonState);
        // We own this if we're suspended (although currently we don't
        // tp_traverse into it; that's a TODO). If we're running, it's
        // empty. If we get deallocated and *still* have a frame, it
        // won't be reachable from the place that normally decref's
        // it, so we need to do it (hence owning it).
        OwnedFrame _top_frame;
#if  GREENLET_USE_CFRAME
        CFrame* cframe;
        int use_tracing;
#endif
        int recursion_depth;

    public:
        PythonState();
        // You can use this for testing whether we have a frame
        // or not. It returns const so they can't modify it.
        const OwnedFrame& top_frame() const G_NOEXCEPT;


        void operator<<(const PyThreadState *const tstate) G_NOEXCEPT;
        void operator>>(PyThreadState* tstate) G_NOEXCEPT;
        void clear() G_NOEXCEPT;

        int tp_traverse(visitproc visit, void* arg, bool visit_top_frame) G_NOEXCEPT;
        void tp_clear(bool own_top_frame) G_NOEXCEPT;
        void set_initial_state(const PyThreadState* const tstate) G_NOEXCEPT;
#if GREENLET_USE_CFRAME
        void set_new_cframe(CFrame& frame) G_NOEXCEPT;
#endif
        void will_switch_from(PyThreadState *const origin_tstate) G_NOEXCEPT;
    };

    class StackState
    {
        // By having only plain C (POD) members, no virtual functions
        // or bases, we get a trivial assignment operator generated
        // for us. However, that's not safe since we do manage memory.
        // So we declare an assignment operator that only works if we
        // don't have any memory allocated. (We don't use
        // std::shared_ptr for reference counting just to keep this
        // object small)
    private:
        char* _stack_start;
        char* stack_stop;
        char* stack_copy;
        intptr_t _stack_saved;
        StackState* stack_prev;
        inline int copy_stack_to_heap_up_to(const char* const stop) G_NOEXCEPT;
        inline void free_stack_copy() G_NOEXCEPT;

    public:
        /**
         * Creates a started, but inactive, state, using *current*
         * as the previous.
         */
        StackState(void* mark, StackState& current);
        /**
         * Creates an inactive, unstarted, state.
         */
        StackState();
        ~StackState();
        StackState(const StackState& other);
        StackState& operator=(const StackState& other);
        inline void copy_heap_to_stack(const StackState& current) G_NOEXCEPT;
        inline int copy_stack_to_heap(char* const stackref, const StackState& current) G_NOEXCEPT;
        inline bool started() const G_NOEXCEPT;
        inline bool main() const G_NOEXCEPT;
        inline bool active() const G_NOEXCEPT;
        inline void set_active() G_NOEXCEPT;
        inline void set_inactive() G_NOEXCEPT;
        inline intptr_t stack_saved() const G_NOEXCEPT;
        inline char* stack_start() const G_NOEXCEPT;
        static inline StackState make_main() G_NOEXCEPT;
        friend std::ostream& operator<<(std::ostream& os, const StackState& s);
    };
    std::ostream& operator<<(std::ostream& os, const StackState& s);

    class SwitchingArgs
    {
    private:
        G_NO_ASSIGNMENT_OF_CLS(SwitchingArgs);
        // If args and kwargs are both false (NULL), this is a *throw*, not a
        // switch. PyErr_... must have been called already.
        OwnedObject _args;
        OwnedObject _kwargs;
    public:

        SwitchingArgs()
        {}

        SwitchingArgs(const OwnedObject& args, const OwnedObject& kwargs)
            : _args(args),
              _kwargs(kwargs)
        {}

        SwitchingArgs(const SwitchingArgs& other)
            : _args(other._args),
              _kwargs(other._kwargs)
        {}

        OwnedObject& args()
        {
            return this->_args;
        }

        OwnedObject& kwargs()
        {
            return this->_kwargs;
        }

        /**
         * Moves ownership from the argument to this object.
         */
        SwitchingArgs& operator<<=(SwitchingArgs& other)
        {
            if (this != &other) {
                this->_args = other._args;
                this->_kwargs = other._kwargs;
                other.CLEAR();
            }
            return *this;
        }

        /**
         * Acquires ownership of the argument (consumes the reference).
         */
        SwitchingArgs& operator<<=(PyObject* args)
        {
            this->_args = OwnedObject::consuming(args);
            this->_kwargs.CLEAR();
            return *this;
        }

        /**
         * Acquires ownership of the argument.
         *
         * Sets the args to be the given value; clears the kwargs.
         */
        SwitchingArgs& operator<<=(OwnedObject& args)
        {
            assert(&args != &this->_args);
            this->_args = args;
            this->_kwargs.CLEAR();
            args.CLEAR();

            return *this;
        }

        G_EXPLICIT_OP operator bool() const G_NOEXCEPT
        {
            return this->_args || this->_kwargs;
        }

        inline void CLEAR()
        {
            this->_args.CLEAR();
            this->_kwargs.CLEAR();
        }
    };

    class ThreadState;

    class UserGreenlet;
    class MainGreenlet;

    class Greenlet
    {
    private:
        G_NO_COPIES_OF_CLS(Greenlet);
    private:
        // XXX: Work to remove these.
        friend class ThreadState;
        friend class UserGreenlet;
        friend class MainGreenlet;
    protected:
        ExceptionState exception_state;
        SwitchingArgs switch_args;
        StackState stack_state;
        PythonState python_state;
        Greenlet(PyGreenlet* p, const StackState& initial_state);
    public:
        Greenlet(PyGreenlet* p);
        virtual ~Greenlet();

        template <typename IsPy37> // maybe we can use a value here?
        const OwnedObject context(const typename IsPy37::IsIt=nullptr) const;

        template <typename IsPy37>
        inline void context(refs::BorrowedObject new_context, typename IsPy37::IsIt=nullptr);

        inline SwitchingArgs& args()
        {
            return this->switch_args;
        }

        virtual const refs::BorrowedMainGreenlet main_greenlet() const = 0;

        inline intptr_t stack_saved() const G_NOEXCEPT
        {
            return this->stack_state.stack_saved();
        }

        // This is used by the macro SLP_SAVE_STATE to compute the
        // difference in stack sizes. It might be nice to handle the
        // computation ourself, but the type of the result
        // varies by platform, so doing it in the macro is the
        // simplest way.
        inline const char* stack_start() const G_NOEXCEPT
        {
            return this->stack_state.stack_start();
        }

        virtual OwnedObject throw_GreenletExit_during_dealloc(const ThreadState& current_thread_state);
        virtual OwnedObject g_switch() = 0;
        /**
         * Force the greenlet to appear dead. Used when it's not
         * possible to throw an exception into a greenlet anymore.
         *
         * This losses access to the thread state and the main greenlet.
         */
        virtual void murder_in_place();

        /**
         * Called when somebody notices we were running in a dead
         * thread to allow cleaning up resources (because we can't
         * raise GreenletExit into it anymore).
         * This is very similar to ``murder_in_place()``, except that
         * it DOES NOT lose the main greenlet or thread state.
         */
        inline void deactivate_and_free();


        // Called when some thread wants to deallocate a greenlet
        // object.
        // The thread may or may not be the same thread the greenlet
        // was running in.
        // The thread state will be null if the thread the greenlet
        // was running in was known to have exited.
        void deallocing_greenlet_in_thread(const ThreadState* current_state);

        // TODO: Figure out how to make these non-public.
        inline void slp_restore_state() G_NOEXCEPT;
        inline int slp_save_state(char *const stackref) G_NOEXCEPT;

        inline bool is_currently_running_in_some_thread() const;
        virtual bool belongs_to_thread(const ThreadState* state) const;

        inline bool started() const
        {
            return this->stack_state.started();
        }
        inline bool active() const
        {
            return this->stack_state.active();
        }
        inline bool main() const
        {
            return this->stack_state.main();
        }
        virtual refs::BorrowedMainGreenlet find_main_greenlet_in_lineage() const = 0;

        virtual const OwnedGreenlet parent() const = 0;
        virtual void parent(const refs::BorrowedObject new_parent) = 0;

        inline const PythonState::OwnedFrame& top_frame()
        {
            return this->python_state.top_frame();
        }

        virtual const OwnedObject& run() const = 0;
        virtual void run(const refs::BorrowedObject nrun) = 0;


        virtual int tp_traverse(visitproc visit, void* arg);
        virtual int tp_clear();


        // Return the thread state that the greenlet is running in, or
        // null if the greenlet is not running or the thread is known
        // to have exited.
        virtual ThreadState* thread_state() const G_NOEXCEPT = 0;

        // Return true if the greenlet is known to have been running
        // (active) in a thread that has now exited.
        virtual bool was_running_in_dead_thread() const G_NOEXCEPT = 0;

        // Return a borrowed greenlet that is the Python object
        // this object represents.
        virtual BorrowedGreenlet self() const G_NOEXCEPT = 0;

    protected:
        inline void release_args();

        // The functions that must not be inlined are declared virtual.
        // We also mark them as protected, not private, so that the
        // compiler is forced to call them through a function pointer.
        // (A sufficiently smart compiler could directly call a private
        // virtual function since it can never be overridden in a
        // subclass).

        // Also TODO: Switch away from integer error codes and to enums,
        // or throw exceptions when possible.
        struct switchstack_result_t
        {
            int status;
            Greenlet* the_state_that_switched;
            OwnedGreenlet origin_greenlet;

            switchstack_result_t()
                : status(0),
                  the_state_that_switched(nullptr)
            {}

            switchstack_result_t(int err)
                : status(err),
                  the_state_that_switched(nullptr)
            {}

            switchstack_result_t(int err, Greenlet* state, OwnedGreenlet& origin)
                : status(err),
                  the_state_that_switched(state),
                  origin_greenlet(origin)
            {
            }

            switchstack_result_t(int err, Greenlet* state, const BorrowedGreenlet& origin)
                : status(err),
                  the_state_that_switched(state),
                  origin_greenlet(origin)
            {
            }

            switchstack_result_t& operator=(const switchstack_result_t& other)
            {
                this->status = other.status;
                this->the_state_that_switched = other.the_state_that_switched;
                this->origin_greenlet = other.origin_greenlet;
                return *this;
            }
        };

        // Returns the previous greenlet we just switched away from.
        virtual OwnedGreenlet g_switchstack_success() G_NOEXCEPT;


        // Check the preconditions for switching to this greenlet; if they
        // aren't met, throws PyErrOccurred. Most callers will want to
        // catch this and clear the arguments
        inline void check_switch_allowed() const;
        class GreenletStartedWhileInPython : public std::runtime_error
        {
        public:
            GreenletStartedWhileInPython() : std::runtime_error("")
            {}
        };

    protected:


        /**
           Perform a stack switch into this greenlet.

           This temporarily sets the global variable
           ``switching_thread_state`` to this greenlet; as soon as the
           call to ``slp_switch`` completes, this is reset to NULL.
           Consequently, this depends on the GIL.

           TODO: Adopt the stackman model and pass ``slp_switch`` a
           callback function and context pointer; this eliminates the
           need for global variables altogether.

           Because the stack switch happens in this function, this
           function can't use its own stack (local) variables, set
           before the switch, and then accessed after the switch.

           Further, you con't even access ``g_thread_state_global``
           before and after the switch from the global variable.
           Because it is thread local some compilers cache it in a
           register/on the stack, notably new versions of MSVC; this
           breaks with strange crashes sometime later, because writing
           to anything in ``g_thread_state_global`` after the switch
           is actually writing to random memory. For this reason, we
           call a non-inlined function to finish the operation. (XXX:
           The ``/GT`` MSVC compiler argument probably fixes that.)

           It is very important that stack switch is 'atomic', i.e. no
           calls into other Python code allowed (except very few that
           are safe), because global variables are very fragile. (This
           should no longer be the case with thread-local variables.)

        */
        switchstack_result_t g_switchstack(void);
    private:
        OwnedObject g_switch_finish(const switchstack_result_t& err);

    };

    class UserGreenlet : public Greenlet
    {
    private:
        static greenlet::PythonAllocator<UserGreenlet> allocator;
        BorrowedGreenlet _self;
        OwnedMainGreenlet _main_greenlet;
        OwnedObject _run_callable;
        OwnedGreenlet _parent;
    public:
        static void* operator new(size_t UNUSED(count));
        static void operator delete(void* ptr);

        UserGreenlet(PyGreenlet* p, BorrowedGreenlet the_parent);
        virtual ~UserGreenlet();

        virtual refs::BorrowedMainGreenlet find_main_greenlet_in_lineage() const;
        virtual bool was_running_in_dead_thread() const G_NOEXCEPT;
        virtual ThreadState* thread_state() const G_NOEXCEPT;
        virtual OwnedObject g_switch();
        virtual const OwnedObject& run() const
        {
            if (this->started() || !this->_run_callable) {
                throw AttributeError("run");
            }
            return this->_run_callable;
        }
        virtual void run(const refs::BorrowedObject nrun);

        virtual const OwnedGreenlet parent() const;
        virtual void parent(const refs::BorrowedObject new_parent);

        virtual const refs::BorrowedMainGreenlet main_greenlet() const;

        virtual BorrowedGreenlet self() const G_NOEXCEPT;
        virtual void murder_in_place();
        virtual bool belongs_to_thread(const ThreadState* state) const;
        virtual int tp_traverse(visitproc visit, void* arg);
        virtual int tp_clear();
        class ParentIsCurrentGuard
        {
        private:
            OwnedGreenlet oldparent;
            UserGreenlet* greenlet;
            G_NO_COPIES_OF_CLS(ParentIsCurrentGuard);
        public:
            ParentIsCurrentGuard(UserGreenlet* p, const ThreadState& thread_state);
            ~ParentIsCurrentGuard();
        };
        virtual OwnedObject throw_GreenletExit_during_dealloc(const ThreadState& current_thread_state);
    protected:
        virtual switchstack_result_t g_initialstub(void* mark);
    private:
        void inner_bootstrap(OwnedGreenlet& origin_greenlet, OwnedObject& run) G_NOEXCEPT;
    };

    class MainGreenlet : public Greenlet
    {
    private:
        static greenlet::PythonAllocator<MainGreenlet> allocator;
        refs::BorrowedMainGreenlet _self;
        ThreadState* _thread_state;
        G_NO_COPIES_OF_CLS(MainGreenlet);
    public:
        static void* operator new(size_t UNUSED(count));
        static void operator delete(void* ptr);

        MainGreenlet(refs::BorrowedMainGreenlet::PyType*, ThreadState*);
        virtual ~MainGreenlet();


        virtual const OwnedObject& run() const;
        virtual void run(const refs::BorrowedObject nrun);

        virtual const OwnedGreenlet parent() const;
        virtual void parent(const refs::BorrowedObject new_parent);

        virtual const refs::BorrowedMainGreenlet main_greenlet() const;

        virtual refs::BorrowedMainGreenlet find_main_greenlet_in_lineage() const;
        virtual bool was_running_in_dead_thread() const G_NOEXCEPT;
        virtual ThreadState* thread_state() const G_NOEXCEPT;
        void thread_state(ThreadState*) G_NOEXCEPT;
        virtual OwnedObject g_switch();
        virtual BorrowedGreenlet self() const G_NOEXCEPT;
        virtual int tp_traverse(visitproc visit, void* arg);
    };

};

template<typename T>
void greenlet::operator<<(const PyThreadState *const lhs, T& rhs)
{
    rhs.operator<<(lhs);
}

using greenlet::ExceptionState;

ExceptionState::ExceptionState()
{
    this->clear();
}

#if PY_VERSION_HEX >= 0x030700A3
// ******** Python 3.7 and above *********
void ExceptionState::operator<<(const PyThreadState *const tstate) G_NOEXCEPT
{
    this->exc_info = tstate->exc_info;
    this->exc_state = tstate->exc_state;
}

void ExceptionState::operator>>(PyThreadState *const tstate) G_NOEXCEPT
{
    tstate->exc_state = this->exc_state;
    tstate->exc_info =
        this->exc_info ? this->exc_info : &tstate->exc_state;
    this->clear();
}

void ExceptionState::clear() G_NOEXCEPT
{
    this->exc_info = nullptr;
    this->exc_state.exc_type = nullptr;
    this->exc_state.exc_value = nullptr;
    this->exc_state.exc_traceback = nullptr;
    this->exc_state.previous_item = nullptr;
}

int ExceptionState::tp_traverse(visitproc visit, void* arg) G_NOEXCEPT
{
    Py_VISIT(this->exc_state.exc_type);
    Py_VISIT(this->exc_state.exc_value);
    Py_VISIT(this->exc_state.exc_traceback);
    return 0;
}

void ExceptionState::tp_clear() G_NOEXCEPT
{
    Py_CLEAR(this->exc_state.exc_type);
    Py_CLEAR(this->exc_state.exc_value);
    Py_CLEAR(this->exc_state.exc_traceback);
}
#else
// ********** Python 3.6 and below ********
void ExceptionState::operator<<(const PyThreadState *const tstate) G_NOEXCEPT
{
    this->exc_type.steal(tstate->exc_type);
    this->exc_value.steal(tstate->exc_value);
    this->exc_traceback.steal(tstate->exc_traceback);
}

void ExceptionState::operator>>(PyThreadState *const tstate) G_NOEXCEPT
{
    tstate->exc_type <<= this->exc_type;
    tstate->exc_value <<= this->exc_value;
    tstate->exc_traceback <<= this->exc_traceback;
    this->clear();
}

void ExceptionState::clear() G_NOEXCEPT
{
    this->exc_type = nullptr;
    this->exc_value = nullptr;
    this->exc_traceback = nullptr;
}

int ExceptionState::tp_traverse(visitproc visit, void* arg) G_NOEXCEPT
{
    Py_VISIT(this->exc_type.borrow());
    Py_VISIT(this->exc_value.borrow());
    Py_VISIT(this->exc_traceback.borrow());
    return 0;
}

void ExceptionState::tp_clear() G_NOEXCEPT
{
    this->exc_type.CLEAR();
    this->exc_value.CLEAR();
    this->exc_traceback.CLEAR();
}
#endif


using greenlet::PythonState;

PythonState::PythonState()
    : _top_frame()
#if GREENLET_USE_CFRAME
    ,cframe(nullptr)
    ,use_tracing(0)
#endif
    ,recursion_depth(0)
{
#if GREENLET_USE_CFRAME
    /*
      The PyThreadState->cframe pointer usually points to memory on
      the stack, alloceted in a call into PyEval_EvalFrameDefault.

      Initially, before any evaluation begins, it points to the
      initial PyThreadState object's ``root_cframe`` object, which is
      statically allocated for the lifetime of the thread.

      A greenlet can last for longer than a call to
      PyEval_EvalFrameDefault, so we can't set its ``cframe`` pointer
      to be the current ``PyThreadState->cframe``; nor could we use
      one from the greenlet parent for the same reason. Yet a further
      no: we can't allocate one scoped to the greenlet and then
      destroy it when the greenlet is deallocated, because inside the
      interpreter the CFrame objects form a linked list, and that too
      can result in accessing memory beyond its dynamic lifetime (if
      the greenlet doesn't actually finish before it dies, its entry
      could still be in the list).

      Using the ``root_cframe`` is problematic, though, because its
      members are never modified by the interpreter and are set to 0,
      meaning that its ``use_tracing`` flag is never updated. We don't
      want to modify that value in the ``root_cframe`` ourself: it
      *shouldn't* matter much because we should probably never get
      back to the point where that's the only cframe on the stack;
      even if it did matter, the major consequence of an incorrect
      value for ``use_tracing`` is that if its true the interpreter
      does some extra work --- however, it's just good code hygiene.

      Our solution: before a greenlet runs, after its initial
      creation, it uses the ``root_cframe`` just to have something to
      put there. However, once the greenlet is actually switched to
      for the first time, ``g_initialstub`` (which doesn't actually
      "return" while the greenlet is running) stores a new CFrame on
      its local stack, and copies the appropriate values from the
      currently running CFrame; this is then made the CFrame for the
      newly-minted greenlet. ``g_initialstub`` then proceeds to call
      ``glet.run()``, which results in ``PyEval_...`` adding the
      CFrame to the list. Switches continue as normal. Finally, when
      the greenlet finishes, the call to ``glet.run()`` returns and
      the CFrame is taken out of the linked list and the stack value
      is now unused and free to expire.

      XXX: I think we can do better. If we're deallocing in the same
      thread, can't we traverse the list and unlink our frame?
      Can we just keep a reference to the thread state in case we
      dealloc in another thread? (Is that even possible if we're still
      running and haven't returned from g_initialstub?)
    */
    this->cframe = &PyThreadState_GET()->root_cframe;
#endif
}

void PythonState::operator<<(const PyThreadState *const tstate) G_NOEXCEPT
{
    this->recursion_depth = tstate->recursion_depth;
    this->_top_frame.steal(tstate->frame);
#if GREENLET_PY37
    this->_context.steal(tstate->context);
#endif
#if GREENLET_USE_CFRAME
    /*
      IMPORTANT: ``cframe`` is a pointer into the STACK. Thus, because
      the call to ``slp_switch()`` changes the contents of the stack,
      you cannot read from ``ts_current->cframe`` after that call and
      necessarily get the same values you get from reading it here.
      Anything you need to restore from now to then must be saved in a
      global/threadlocal variable (because we can't use stack
      variables here either). For things that need to persist across
      the switch, use `will_switch_from`.
    */
    this->cframe = tstate->cframe;
    this->use_tracing = tstate->cframe->use_tracing;
#endif
}

void PythonState::operator>>(PyThreadState *const tstate) G_NOEXCEPT
{
    tstate->recursion_depth = this->recursion_depth;
    tstate->frame = this->_top_frame.relinquish_ownership();
#if GREENLET_PY37
    tstate->context = this->_context.relinquish_ownership();
    /* Incrementing this value invalidates the contextvars cache,
       which would otherwise remain valid across switches */
    tstate->context_ver++;
#endif
#if GREENLET_USE_CFRAME
    tstate->cframe = this->cframe;
    /*
      If we were tracing, we need to keep tracing.
      There should never be the possibility of hitting the
      root_cframe here. See note above about why we can't
      just copy this from ``origin->cframe->use_tracing``.
    */
    tstate->cframe->use_tracing = this->use_tracing;
#endif
}

void PythonState::will_switch_from(PyThreadState *const origin_tstate) G_NOEXCEPT
{
#if GREENLET_USE_CFRAME
    // The weird thing is, we don't actually save this for an
    // effect on the current greenlet, it's saved for an
    // effect on the target greenlet. That is, we want
    // continuity of this setting across the greenlet switch.
    this->use_tracing = origin_tstate->cframe->use_tracing;
#endif
}

void PythonState::set_initial_state(const PyThreadState* const tstate) G_NOEXCEPT
{
    this->_top_frame = nullptr;
    this->recursion_depth = tstate->recursion_depth;
}
// TODO: Better state management about when we own the top frame.
int PythonState::tp_traverse(visitproc visit, void* arg, bool own_top_frame) G_NOEXCEPT
{
#if GREENLET_PY37
    Py_VISIT(this->_context.borrow());
#endif
    if (own_top_frame) {
        Py_VISIT(this->_top_frame.borrow());
    }
    return 0;
}

void PythonState::tp_clear(bool own_top_frame) G_NOEXCEPT
{
    PythonStateContext::tp_clear();
    // If we get here owning a frame,
    // we got dealloc'd without being finished. We may or may not be
    // in the same thread.
    if (own_top_frame) {
        this->_top_frame.CLEAR();
    }
}

#if GREENLET_USE_CFRAME
void PythonState::set_new_cframe(CFrame& frame) G_NOEXCEPT
{
    frame = *PyThreadState_GET()->cframe;
    /* Make the target greenlet refer to the stack value. */
    this->cframe = &frame;
    /*
      And restore the link to the previous frame so this one gets
      unliked appropriately.
    */
    this->cframe->previous = &PyThreadState_GET()->root_cframe;
}
#endif


const PythonState::OwnedFrame& PythonState::top_frame() const G_NOEXCEPT
{
    return this->_top_frame;
}


using greenlet::StackState;
#include <iostream>
using std::cerr;
using std::endl;

std::ostream& greenlet::operator<<(std::ostream& os, const StackState& s)
{
    os << "StackState(stack_start=" << (void*)s._stack_start
       << ", stack_stop=" << (void*)s.stack_stop
       << ", stack_copy=" << (void*)s.stack_copy
       << ", stack_saved=" << s._stack_saved
       << ", stack_prev=" << s.stack_prev
       << ", addr=" << &s
       << ")";
    return os;
}

StackState::StackState(void* mark, StackState& current)
    : _stack_start(nullptr),
      stack_stop((char*)mark),
      stack_copy(nullptr),
      _stack_saved(0),
      /* Skip a dying greenlet */
      stack_prev(current._stack_start
                 ? &current
                 : current.stack_prev)
{
}

StackState::StackState()
    : _stack_start(nullptr),
      stack_stop(nullptr),
      stack_copy(nullptr),
      _stack_saved(0),
      stack_prev(nullptr)
{
}

StackState::StackState(const StackState& other)
// can't use a delegating constructor because of
// MSVC for Python 2.7
    : _stack_start(nullptr),
      stack_stop(nullptr),
      stack_copy(nullptr),
      _stack_saved(0),
      stack_prev(nullptr)
{
    this->operator=(other);
}

StackState& StackState::operator=(const StackState& other)
{
    if (&other == this) {
        return *this;
    }
    if (other._stack_saved) {
        throw std::runtime_error("Refusing to steal memory.");
    }

    //If we have memory allocated, dispose of it
    this->free_stack_copy();

    this->_stack_start = other._stack_start;
    this->stack_stop = other.stack_stop;
    this->stack_copy = other.stack_copy;
    this->_stack_saved = other._stack_saved;
    this->stack_prev = other.stack_prev;
    return *this;
}

inline void StackState::free_stack_copy() G_NOEXCEPT
{
    PyMem_Free(this->stack_copy);
    this->stack_copy = nullptr;
    this->_stack_saved = 0;
}

inline void StackState::copy_heap_to_stack(const StackState& current) G_NOEXCEPT
{
    // cerr << "copy_heap_to_stack" << endl
    //      << "\tFrom    : " << *this << endl
    //      << "\tCurrent:" << current
    //      << endl;
    /* Restore the heap copy back into the C stack */
    if (this->_stack_saved != 0) {
        memcpy(this->_stack_start, this->stack_copy, this->_stack_saved);
        this->free_stack_copy();
    }
    StackState* owner = const_cast<StackState*>(&current);
    if (!owner->_stack_start) {
        owner = owner->stack_prev; /* greenlet is dying, skip it */
    }
    while (owner && owner->stack_stop <= this->stack_stop) {
        // cerr << "\tOwner: " << owner << endl;
        owner = owner->stack_prev; /* find greenlet with more stack */
    }
    this->stack_prev = owner;
    // cerr << "\tFinished with: " << *this << endl;
}

inline int StackState::copy_stack_to_heap_up_to(const char* const stop) G_NOEXCEPT
{
    /* Save more of g's stack into the heap -- at least up to 'stop'
       g->stack_stop |________|
                     |        |
                     |    __ stop       . . . . .
                     |        |    ==>  .       .
                     |________|          _______
                     |        |         |       |
                     |        |         |       |
      g->stack_start |        |         |_______| g->stack_copy
     */
    intptr_t sz1 = this->_stack_saved;
    intptr_t sz2 = stop - this->_stack_start;
    assert(this->_stack_start);
    if (sz2 > sz1) {
        char* c = (char*)PyMem_Realloc(this->stack_copy, sz2);
        if (!c) {
            PyErr_NoMemory();
            return -1;
        }
        memcpy(c + sz1, this->_stack_start + sz1, sz2 - sz1);
        this->stack_copy = c;
        this->_stack_saved = sz2;
    }
    return 0;
}

inline int StackState::copy_stack_to_heap(char* const stackref,
                                          const StackState& current) G_NOEXCEPT
{
    // cerr << "copy_stack_to_heap: " << endl
    //      << "\tstackref: " << (void*)stackref << endl
    //      << "\tthis: " << *this << endl
    //      << "\tcurrent: " << current
    //      << endl;
    /* must free all the C stack up to target_stop */
    const char* const target_stop = this->stack_stop;

    StackState* owner = const_cast<StackState*>(&current);
    assert(owner->_stack_saved == 0); // everything is present on the stack
    if (!owner->_stack_start) {
        // cerr << "\tcurrent is dead; using: " << owner->stack_prev << endl;
        owner = owner->stack_prev; /* not saved if dying */
    }
    else {
        owner->_stack_start = stackref;
    }

    while (owner->stack_stop < target_stop) {
        // cerr << "\tCopying from " << *owner << endl;
        /* ts_current is entierely within the area to free */
        if (owner->copy_stack_to_heap_up_to(owner->stack_stop)) {
            return -1; /* XXX */
        }
        owner = owner->stack_prev;
    }
    if (owner != this) {
        if (owner->copy_stack_to_heap_up_to(target_stop)) {
            return -1; /* XXX */
        }
    }
    return 0;
}

inline bool StackState::started() const G_NOEXCEPT
{
    return this->stack_stop != nullptr;
}

inline bool StackState::main() const G_NOEXCEPT
{
    return this->stack_stop == (char*)-1;
}

inline bool StackState::active() const G_NOEXCEPT
{
    return this->_stack_start != nullptr;
}

inline void StackState::set_active() G_NOEXCEPT
{
    assert(this->_stack_start == nullptr);
    this->_stack_start = (char*)1;
}

inline void StackState::set_inactive() G_NOEXCEPT
{
    this->_stack_start = nullptr;
    // XXX: What if we still have memory out there?
    // That case is actually triggered by
    // test_issue251_issue252_explicit_reference_not_collectable (greenlet.tests.test_leaks.TestLeaks)
    // and
    // test_issue251_issue252_need_to_collect_in_background
    // (greenlet.tests.test_leaks.TestLeaks)
    //
    // Those objects never get deallocated, so the destructor never
    // runs.
    // It *seems* safe to clean up the memory here?
    if (this->_stack_saved) {
        this->free_stack_copy();
    }
}

inline intptr_t StackState::stack_saved() const G_NOEXCEPT
{
    return this->_stack_saved;
}

inline char* StackState::stack_start() const G_NOEXCEPT
{
    return this->_stack_start;
}


inline StackState StackState::make_main() G_NOEXCEPT
{
    StackState s;
    s._stack_start = (char*)1;
    s.stack_stop = (char*)-1;
    return s;
}

StackState::~StackState()
{
    if (this->_stack_saved != 0) {
        this->free_stack_copy();
    }
}

using greenlet::Greenlet;

bool Greenlet::is_currently_running_in_some_thread() const
{
    return this->stack_state.active() && !this->python_state.top_frame();
}



#endif
