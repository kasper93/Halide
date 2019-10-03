#include "AddAtomicMutex.h"
#include "ExprUsesVar.h"
#include "Func.h"
#include "IREquality.h"
#include "IRMutator.h"
#include "IROperator.h"
#include "OutputImageParam.h"

namespace Halide {
namespace Internal {

using std::map;
using std::set;
using std::string;

namespace {

/** Search if the value of a Store node has a variable pointing to a let binding,
    where the let binding contains the Store location. Use for checking whether
    we need a mutex lock for Atomic since some lowering pass before lifted a let
    binding from the Store node (currently only SplitTuple would do this). */
class FindAtomicLetBindings : public IRGraphVisitor {
public:
    using IRVisitor::visit;

    FindAtomicLetBindings(const Scope<void> &store_names)
        : store_names(store_names) {}

    bool found = false;

protected:
    void visit(const Let *op) override {
        include(op->value);
        {
            ScopedBinding<Expr> bind(let_bindings, op->name, op->value);
            include(op->body);
        }
    }

    void visit(const LetStmt *op) override {
        include(op->value);
        {
            ScopedBinding<Expr> bind(let_bindings, op->name, op->value);
            include(op->body);
        }
    }

    void visit(const Variable *op) override {
        if (!inside_store.empty()) {
            // If this Variable inside the store value is an expression
            // that depends on one of the store_names, we found a lifted let.
            if (expr_uses_vars(op, store_names, let_bindings)) {
                found = true;
            }
        }
    }

    void visit(const Store *op) override {
        include(op->predicate);
        if (store_names.contains(op->name)) {
            // If we are in a designated store and op->value has a let binding
            // that uses one of the store_names, we found a lifted let.
            ScopedValue<string> old_inside_store(inside_store, op->name);
            include(op->value);
        } else {
            include(op->value);
        }
        include(op->index);
    }

    std::string inside_store;
    const Scope<void> &store_names;
    Scope<Expr> let_bindings;
};

/** Collect all store's names inside a statement. */
class CollectStoreNames : public IRGraphVisitor {
public:
    Scope<void> store_names;

protected:
    using IRGraphVisitor::visit;
    
    void visit(const Store *op) override {
        include(op->predicate);
        include(op->value);
        include(op->index);
        store_names.push(op->name);
    }
};

/** Clear out the Atomic node's mutex usages if it doesn't need one. */
class RemoveUnnecessaryMutexUse : public IRMutator {
public:
    set<string> remove_mutex_lock_names;

protected:
    using IRMutator::visit;

    Stmt visit(const Atomic *op) override {
        // Collect the names of all Store nodes inside.
        CollectStoreNames collector;
        op->body.accept(&collector);
        // Search for let bindings that access the producers.
        FindAtomicLetBindings finder(collector.store_names);
        op->body.accept(&finder);
        if (finder.found) {
            // Can't remove mutex lock. Leave the Stmt as is.
            return IRMutator::visit(op);
        } else {
            remove_mutex_lock_names.insert(op->mutex_name);
            Stmt body = mutate(op->body);
            return Atomic::make(op->producer_name,
                                string(),
                                std::move(body));
        }
    }
};

/** Find Store inside an Atomic that matches the provided store_names. */
class FindStoreInAtomicMutex : public IRGraphVisitor {
public:
    using IRGraphVisitor::visit;

    FindStoreInAtomicMutex(const std::set<std::string> &store_names)
        : store_names(store_names) {}

    bool found = false;
    std::string producer_name;
    std::string mutex_name;
protected:
    void visit(const Atomic *op) override {
        if (!found && !op->mutex_name.empty()) {
            ScopedValue<bool> old_in_atomic_mutex(in_atomic_mutex, true);
            include(op->body);
            if (found) {
                // We found a Store inside Atomic with matching name,
                // record the mutex information.
                producer_name = op->producer_name;
                mutex_name = op->mutex_name;
            }
        } else {
            include(op->body);
        }
    }

    void visit(const Store *op) override {
        if (in_atomic_mutex) {
            if (store_names.find(op->name) != store_names.end()) {
                found = true;
            }
        }
        IRGraphVisitor::visit(op);
    }

    bool in_atomic_mutex = false;
    const std::set<std::string> &store_names;
};

/** Find Store inside of an Atomic node and return their indices. */
class FindStoreIndex : public IRGraphVisitor {
public:
    Expr index;
protected:
    using IRGraphVisitor::visit;

    void visit(const Store *op) override {
        // Ideally we want to insert equal() checks here for different stores,
        // but the indices of them actually are different in the case of tuples,
        // since they usually refer to the strides/min/extents of their own tuple
        // buffers. However, different elements in a tuple would have the same
        // strides/min/extents so we are fine.
        if (index.defined()) {
            return;
        }
        index = op->index;
    }
};

/** Add mutex allocation & lock & unlock if required. */
class AddAtomicMutex : public IRMutator {
public:
    AddAtomicMutex(const map<string, Function> &env)
        : env(env) {}

protected:
    using IRMutator::visit;

    const map<string, Function> &env;
    // The set of producers that have allocated a mutex buffer
    set<string> allocated_mutexes;

    Stmt allocate_mutex(const string &mutex_name, Expr extent, Stmt body) {
        Expr mutex_array = Call::make(type_of<halide_mutex_array *>(),
                                      "halide_mutex_array_create",
                                      {extent},
                                      Call::Extern);
        // Allocate a scalar of halide_mutex_array.
        // This generate halide_mutex_array mutex[1];
        body = Allocate::make(mutex_name,
                              Handle(),
                              MemoryType::Stack,
                              {},
                              const_true(),
                              body,
                              mutex_array,
                              "halide_mutex_array_destroy");
        return body;
    }

    Stmt visit(const Allocate *op) override {
        // If this Allocate node is allocating a buffer for a producer,
        // and there is a Store node inside of an Atomic node requiring mutex lock
        // matching the name of the Allocate, allocate a mutex lock.
        FindStoreInAtomicMutex finder({op->name});
        op->body.accept(&finder);
        if (!finder.found) {
            // No Atomic node that requires mutex lock from this node inside.
            return IRMutator::visit(op);
        }

        if (allocated_mutexes.find(finder.mutex_name) != allocated_mutexes.end()) {
            // We've already allocated a mutex.
            return IRMutator::visit(op);
        }

        allocated_mutexes.insert(finder.mutex_name);

        const string &mutex_name = finder.mutex_name;
        Stmt body = mutate(op->body);
        Expr extent = Expr(1);
        for (Expr e : op->extents) {
            extent = extent * e;
        }
        body = allocate_mutex(mutex_name, extent, body);
        return Allocate::make(op->name,
                              op->type,
                              op->memory_type,
                              op->extents,
                              op->condition,
                              std::move(body),
                              op->new_expr,
                              op->free_function);
    }

    Stmt visit(const ProducerConsumer *op) override {
        // Usually we allocate the mutex buffer at the Allocate node,
        // but outputs don't have Allocate. For those we allocate the mutex
        // buffer at the producer node.

        if (!op->is_producer) {
            // This is a consumer.
            return IRMutator::visit(op);
        }

        // Find the corresponding output.
        auto func_it = env.find(op->name);
        Func f = Func(func_it->second);
        internal_assert(f.output_buffers().size() > 0) <<
            "Found a producer node that contains an atomic node that requires mutex lock, "
            "but does not have an Allocate node and is not an output function. This is not supported.\n";

        set<string> store_names;
        for (auto buffer : f.output_buffers()) {
            store_names.insert(buffer.name());
        }

        FindStoreInAtomicMutex finder(store_names);
        op->body.accept(&finder);
        if (!finder.found) {
            // No Atomic node that requires mutex lock from this node inside.
            return IRMutator::visit(op);
        }

        if (allocated_mutexes.find(finder.mutex_name) != allocated_mutexes.end()) {
            // We've already allocated a mutex.
            return IRMutator::visit(op);
        }

        allocated_mutexes.insert(finder.mutex_name);

        // We assume all output buffers in a Tuple have the same extent.
        OutputImageParam output_buffer = f.output_buffers()[0];
        Expr extent = Expr(1);
        for (int i = 0; i < output_buffer.dimensions(); i++) {
            extent = extent * output_buffer.dim(i).extent();
        }
        Stmt body = mutate(op->body);
        body = allocate_mutex(finder.mutex_name, extent, body);
        return ProducerConsumer::make(op->name, op->is_producer, std::move(body));
    }

    Stmt visit(const Atomic *op) override {
        if (op->mutex_name.empty()) {
            return IRMutator::visit(op);
        }

        // Lock the mutexes using the indices from the Store nodes inside the body.
        FindStoreIndex find;
        op->body.accept(&find);

        Expr index = find.index;
        if (!index.defined()) {
            // scalar output
            index = Expr(0);
        }
        Stmt body = op->body;
        // This generates a pointer to the mutex array
        Expr mutex_array = Variable::make(
            type_of<halide_mutex_array *>(), op->mutex_name);
        // Add mutex locks & unlocks
        // If a thread locks the mutex and throws an exception,
        // halide_mutex_array_destroy will be called and cleanup the mutex locks.
        body = Block::make(
            Evaluate::make(Call::make(type_of<int>(),
                                      "halide_mutex_array_lock",
                                      {mutex_array, index},
                                      Call::CallType::Extern)),
            Block::make(std::move(body),
                Evaluate::make(Call::make(type_of<int>(),
                                          "halide_mutex_array_unlock",
                                          {mutex_array, index},
                                          Call::CallType::Extern))));

        return Atomic::make(op->producer_name,
                            op->mutex_name,
                            std::move(body));
    }
};

}  // namespace

Stmt add_atomic_mutex(Stmt s, const map<string, Function> &env) {
    s = RemoveUnnecessaryMutexUse().mutate(s);
    s = AddAtomicMutex(env).mutate(s);
    return s;
}

}  // namespace Internal
}  // namespace Halide