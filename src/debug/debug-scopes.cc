// Copyright 2015 the V8 project authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "src/debug/debug-scopes.h"

#include <memory>

#include "src/ast/ast.h"
#include "src/ast/scopes.h"
#include "src/debug/debug.h"
#include "src/frames-inl.h"
#include "src/globals.h"
#include "src/isolate-inl.h"
#include "src/objects/module.h"
#include "src/parsing/parse-info.h"
#include "src/parsing/parsing.h"
#include "src/parsing/rewriter.h"

namespace v8 {
namespace internal {

ScopeIterator::ScopeIterator(Isolate* isolate, FrameInspector* frame_inspector,
                             ScopeIterator::Option option)
    : isolate_(isolate),
      frame_inspector_(frame_inspector),
      function_(frame_inspector_->GetFunction()),
      script_(frame_inspector_->GetScript()),
      seen_script_scope_(false) {
  if (!frame_inspector->GetContext()->IsContext()) {
    // Optimized frame, context or function cannot be materialized. Give up.
    return;
  }
  context_ = Handle<Context>::cast(frame_inspector->GetContext());

  context_ = Handle<Context>::cast(frame_inspector->GetContext());

  // We should not instantiate a ScopeIterator for wasm frames.
  DCHECK(frame_inspector->GetScript()->type() != Script::TYPE_WASM);

  TryParseAndRetrieveScopes(option);
}

Handle<Object> ScopeIterator::GetFunctionDebugName() const {
  if (HasNestedScopeChain()) return JSFunction::GetDebugName(function_);
  if (!context_->IsNativeContext()) {
    DisallowHeapAllocation no_gc;
    ScopeInfo* closure_info = context_->closure_context()->scope_info();
    Handle<String> debug_name(closure_info->FunctionDebugName());
    if (debug_name->length() > 0) return debug_name;
  }
  return isolate_->factory()->undefined_value();
}

ScopeIterator::ScopeIterator(Isolate* isolate, Handle<JSFunction> function)
    : isolate_(isolate),
      context_(function->context()),
      script_(Script::cast(function->shared()->script())),
      seen_script_scope_(false) {
  if (!function->shared()->IsSubjectToDebugging()) context_ = Handle<Context>();
  UnwrapEvaluationContext();
}

ScopeIterator::ScopeIterator(Isolate* isolate,
                             Handle<JSGeneratorObject> generator)
    : isolate_(isolate),
      generator_(generator),
      function_(generator->function()),
      context_(generator->context()),
      script_(Script::cast(function_->shared()->script())),
      seen_script_scope_(false) {
  if (!function_->shared()->IsSubjectToDebugging()) {
    context_ = Handle<Context>();
    return;
  }
  TryParseAndRetrieveScopes(DEFAULT);
}

void ScopeIterator::TryParseAndRetrieveScopes(ScopeIterator::Option option) {
  // Catch the case when the debugger stops in an internal function.
  Handle<SharedFunctionInfo> shared_info(function_->shared());
  Handle<ScopeInfo> scope_info(shared_info->scope_info());
  if (shared_info->script()->IsUndefined(isolate_)) {
    context_ = handle(function_->context());
    function_ = Handle<JSFunction>();
    return;
  }

  // Currently it takes too much time to find nested scopes due to script
  // parsing. Sometimes we want to run the ScopeIterator as fast as possible
  // (for example, while collecting async call stacks on every
  // addEventListener call), even if we drop some nested scopes.
  // Later we may optimize getting the nested scopes (cache the result?)
  // and include nested scopes into the "fast" iteration case as well.
  bool ignore_nested_scopes = (option == IGNORE_NESTED_SCOPES);
  bool collect_non_locals = (option == COLLECT_NON_LOCALS);
  if (!ignore_nested_scopes && shared_info->HasBreakInfo() &&
      frame_inspector_ != nullptr) {
    // The source position at return is always the end of the function,
    // which is not consistent with the current scope chain. Therefore all
    // nested with, catch and block contexts are skipped, and we can only
    // inspect the function scope.
    // This can only happen if we set a break point inside right before the
    // return, which requires a debug info to be available.
    Handle<DebugInfo> debug_info(shared_info->GetDebugInfo());

    // Find the break point where execution has stopped.
    BreakLocation location = BreakLocation::FromFrame(debug_info, GetFrame());

    ignore_nested_scopes = location.IsReturn();
  }

  if (ignore_nested_scopes) {
    if (scope_info->HasContext()) {
      context_ = Handle<Context>(context_->declaration_context(), isolate_);
    } else {
      context_ = handle(function_->context());
    }
    if (scope_info->scope_type() == FUNCTION_SCOPE) {
      nested_scope_chain_.emplace_back(scope_info, shared_info->StartPosition(),
                                       shared_info->EndPosition());
    }
    if (!collect_non_locals) return;
  }

  // Reparse the code and analyze the scopes.
  // Check whether we are in global, eval or function code.
  std::unique_ptr<ParseInfo> info;
  if (scope_info->scope_type() != FUNCTION_SCOPE) {
    // Global or eval code.
    Handle<Script> script(Script::cast(shared_info->script()));
    info.reset(new ParseInfo(isolate_, script));
    if (scope_info->scope_type() == EVAL_SCOPE) {
      info->set_eval();
      if (!function_->context()->IsNativeContext()) {
        info->set_outer_scope_info(handle(function_->context()->scope_info()));
      }
      // Language mode may be inherited from the eval caller.
      // Retrieve it from shared function info.
      info->set_language_mode(shared_info->language_mode());
    } else if (scope_info->scope_type() == MODULE_SCOPE) {
      DCHECK(info->is_module());
    } else {
      DCHECK(scope_info->scope_type() == SCRIPT_SCOPE);
    }
  } else {
    // Inner function.
    info.reset(new ParseInfo(isolate_, shared_info));
  }
  if (parsing::ParseAny(info.get(), shared_info, isolate_) &&
      Rewriter::Rewrite(info.get())) {
    info->ast_value_factory()->Internalize(isolate_);
    DeclarationScope* scope = info->literal()->scope();
    if (!ignore_nested_scopes || collect_non_locals) {
      CollectNonLocals(info.get(), scope);
    }
    if (!ignore_nested_scopes) {
      if (DeclarationScope::Analyze(info.get())) {
        DeclarationScope::AllocateScopeInfos(info.get(), isolate_,
                                             AnalyzeMode::kDebugger);
        RetrieveScopeChain(scope);
      }
    }
  } else {
    // A failed reparse indicates that the preparser has diverged from the
    // parser or that the preparse data given to the initial parse has been
    // faulty. We fail in debug mode but in release mode we only provide the
    // information we get from the context chain but nothing about
    // completely stack allocated scopes or stack allocated locals.
    // Or it could be due to stack overflow.
    // Silently fail by presenting an empty context chain.
    CHECK(isolate_->has_pending_exception());
    isolate_->clear_pending_exception();
    context_ = Handle<Context>();
  }
  UnwrapEvaluationContext();
}

void ScopeIterator::UnwrapEvaluationContext() {
  while (true) {
    if (context_.is_null()) return;
    if (!context_->IsDebugEvaluateContext()) return;
    Handle<Object> wrapped(context_->get(Context::WRAPPED_CONTEXT_INDEX),
                           isolate_);
    if (wrapped->IsContext()) {
      context_ = Handle<Context>::cast(wrapped);
    } else {
      context_ = Handle<Context>(context_->previous(), isolate_);
    }
  }
}

V8_WARN_UNUSED_RESULT MaybeHandle<JSObject>
ScopeIterator::MaterializeScopeDetails() {
  // Calculate the size of the result.
  Handle<FixedArray> details =
      isolate_->factory()->NewFixedArray(kScopeDetailsSize);
  // Fill in scope details.
  details->set(kScopeDetailsTypeIndex, Smi::FromInt(Type()));
  Handle<JSObject> scope_object;
  ASSIGN_RETURN_ON_EXCEPTION(isolate_, scope_object, ScopeObject(), JSObject);
  details->set(kScopeDetailsObjectIndex, *scope_object);
  if (Type() == ScopeTypeGlobal || Type() == ScopeTypeScript) {
    return isolate_->factory()->NewJSArrayWithElements(details);
  } else if (HasContext()) {
    Handle<Object> closure_name = GetFunctionDebugName();
    details->set(kScopeDetailsNameIndex, *closure_name);
    details->set(kScopeDetailsStartPositionIndex,
                 Smi::FromInt(start_position()));
    details->set(kScopeDetailsEndPositionIndex, Smi::FromInt(end_position()));
    if (HasNestedScopeChain()) {
      details->set(kScopeDetailsFunctionIndex, *function_);
    }
  }
  return isolate_->factory()->NewJSArrayWithElements(details);
}

bool ScopeIterator::HasPositionInfo() {
  return HasNestedScopeChain() || !context_->IsNativeContext();
}

int ScopeIterator::start_position() {
  if (HasNestedScopeChain()) {
    return LastNestedScopeChain().start_position;
  }
  if (context_->IsNativeContext()) return 0;
  return context_->closure_context()->scope_info()->StartPosition();
}

int ScopeIterator::end_position() {
  if (HasNestedScopeChain()) {
    return LastNestedScopeChain().end_position;
  }
  if (context_->IsNativeContext()) return 0;
  return context_->closure_context()->scope_info()->EndPosition();
}

void ScopeIterator::Next() {
  DCHECK(!Done());
  ScopeType scope_type = Type();
  if (scope_type == ScopeTypeGlobal) {
    // The global scope is always the last in the chain.
    DCHECK(context_->IsNativeContext());
    context_ = Handle<Context>();
  } else if (scope_type == ScopeTypeScript) {
    seen_script_scope_ = true;
    if (context_->IsScriptContext()) {
      context_ = Handle<Context>(context_->previous(), isolate_);
    }
    if (HasNestedScopeChain()) {
      DCHECK_EQ(LastNestedScopeChain().scope_info->scope_type(), SCRIPT_SCOPE);
      nested_scope_chain_.pop_back();
      DCHECK(!HasNestedScopeChain());
    }
    CHECK(context_->IsNativeContext());
  } else if (!HasNestedScopeChain()) {
    context_ = Handle<Context>(context_->previous(), isolate_);
  } else {
    do {
      if (LastNestedScopeChain().scope_info->HasContext()) {
        DCHECK(context_->previous() != nullptr);
        context_ = Handle<Context>(context_->previous(), isolate_);
      }
      nested_scope_chain_.pop_back();
      if (!HasNestedScopeChain()) break;
      // Repeat to skip hidden scopes.
    } while (LastNestedScopeChain().is_hidden());
  }
  if (!HasNestedScopeChain()) function_ = Handle<JSFunction>();
  UnwrapEvaluationContext();
}


// Return the type of the current scope.
ScopeIterator::ScopeType ScopeIterator::Type() {
  DCHECK(!Done());
  if (HasNestedScopeChain()) {
    Handle<ScopeInfo> scope_info = LastNestedScopeChain().scope_info;
    switch (scope_info->scope_type()) {
      case FUNCTION_SCOPE:
        DCHECK(context_->IsFunctionContext() || !scope_info->HasContext());
        return ScopeTypeLocal;
      case MODULE_SCOPE:
        DCHECK(context_->IsModuleContext());
        return ScopeTypeModule;
      case SCRIPT_SCOPE:
        DCHECK(context_->IsScriptContext() || context_->IsNativeContext());
        return ScopeTypeScript;
      case WITH_SCOPE:
        DCHECK(context_->IsWithContext() || context_->IsDebugEvaluateContext());
        return ScopeTypeWith;
      case CATCH_SCOPE:
        DCHECK(context_->IsCatchContext());
        return ScopeTypeCatch;
      case BLOCK_SCOPE:
        DCHECK(!scope_info->HasContext() || context_->IsBlockContext());
        return ScopeTypeBlock;
      case EVAL_SCOPE:
        DCHECK(!scope_info->HasContext() || context_->IsEvalContext());
        return ScopeTypeEval;
    }
    UNREACHABLE();
  }
  if (context_->IsNativeContext()) {
    DCHECK(context_->global_object()->IsJSGlobalObject());
    // If we are at the native context and have not yet seen script scope,
    // fake it.
    return seen_script_scope_ ? ScopeTypeGlobal : ScopeTypeScript;
  }
  if (context_->IsFunctionContext() || context_->IsEvalContext()) {
    return ScopeTypeClosure;
  }
  if (context_->IsCatchContext()) {
    return ScopeTypeCatch;
  }
  if (context_->IsBlockContext()) {
    return ScopeTypeBlock;
  }
  if (context_->IsModuleContext()) {
    return ScopeTypeModule;
  }
  if (context_->IsScriptContext()) {
    return ScopeTypeScript;
  }
  DCHECK(context_->IsWithContext() || context_->IsDebugEvaluateContext());
  return ScopeTypeWith;
}


MaybeHandle<JSObject> ScopeIterator::ScopeObject() {
  DCHECK(!Done());
  switch (Type()) {
    case ScopeIterator::ScopeTypeGlobal:
      return Handle<JSObject>(CurrentContext()->global_proxy());
    case ScopeIterator::ScopeTypeScript:
      return MaterializeScriptScope();
    case ScopeIterator::ScopeTypeLocal:
      // Materialize the content of the local scope into a JSObject.
      DCHECK_EQ(1, nested_scope_chain_.size());
      return MaterializeLocalScope();
    case ScopeIterator::ScopeTypeWith:
      return WithContextExtension();
    case ScopeIterator::ScopeTypeClosure:
      // Materialize the content of the closure scope into a JSObject.
      return MaterializeClosure();
    case ScopeIterator::ScopeTypeCatch:
    case ScopeIterator::ScopeTypeBlock:
    case ScopeIterator::ScopeTypeEval:
      return MaterializeInnerScope();
    case ScopeIterator::ScopeTypeModule:
      return MaterializeModuleScope();
  }
  UNREACHABLE();
}


bool ScopeIterator::HasContext() {
  ScopeType type = Type();
  if (type == ScopeTypeBlock || type == ScopeTypeLocal ||
      type == ScopeTypeEval) {
    if (HasNestedScopeChain()) {
      return LastNestedScopeChain().scope_info->HasContext();
    }
  }
  return true;
}


bool ScopeIterator::SetVariableValue(Handle<String> variable_name,
                                     Handle<Object> new_value) {
  DCHECK(!Done());
  switch (Type()) {
    case ScopeIterator::ScopeTypeGlobal:
      break;
    case ScopeIterator::ScopeTypeLocal:
      return SetLocalVariableValue(variable_name, new_value);
    case ScopeIterator::ScopeTypeWith:
      break;
    case ScopeIterator::ScopeTypeClosure:
      return SetClosureVariableValue(variable_name, new_value);
    case ScopeIterator::ScopeTypeScript:
      return SetScriptVariableValue(variable_name, new_value);
    case ScopeIterator::ScopeTypeCatch:
    case ScopeIterator::ScopeTypeBlock:
    case ScopeIterator::ScopeTypeEval:
      return SetInnerScopeVariableValue(variable_name, new_value);
    case ScopeIterator::ScopeTypeModule:
      return SetModuleVariableValue(variable_name, new_value);
      break;
  }
  return false;
}


Handle<ScopeInfo> ScopeIterator::CurrentScopeInfo() {
  DCHECK(!Done());
  if (HasNestedScopeChain()) {
    return LastNestedScopeChain().scope_info;
  } else if (context_->IsBlockContext() || context_->IsFunctionContext() ||
             context_->IsEvalContext() || context_->IsCatchContext()) {
    return Handle<ScopeInfo>(context_->scope_info());
  }
  return Handle<ScopeInfo>::null();
}


Handle<Context> ScopeIterator::CurrentContext() {
  DCHECK(!Done());
  if (Type() == ScopeTypeGlobal || Type() == ScopeTypeScript ||
      !HasNestedScopeChain()) {
    return context_;
  } else if (LastNestedScopeChain().scope_info->HasContext()) {
    return context_;
  } else {
    return Handle<Context>::null();
  }
}

Handle<StringSet> ScopeIterator::GetNonLocals() { return non_locals_; }

#ifdef DEBUG
// Debug print of the content of the current scope.
void ScopeIterator::DebugPrint() {
  OFStream os(stdout);
  DCHECK(!Done());
  switch (Type()) {
    case ScopeIterator::ScopeTypeGlobal:
      os << "Global:\n";
      CurrentContext()->Print(os);
      break;

    case ScopeIterator::ScopeTypeLocal: {
      os << "Local:\n";
      function_->shared()->scope_info()->Print();
      if (!CurrentContext().is_null()) {
        CurrentContext()->Print(os);
        if (CurrentContext()->has_extension()) {
          Handle<HeapObject> extension(CurrentContext()->extension(), isolate_);
          if (extension->IsJSContextExtensionObject()) {
            extension->Print(os);
          }
        }
      }
      break;
    }

    case ScopeIterator::ScopeTypeWith:
      os << "With:\n";
      CurrentContext()->extension()->Print(os);
      break;

    case ScopeIterator::ScopeTypeCatch:
      os << "Catch:\n";
      CurrentContext()->extension()->Print(os);
      CurrentContext()->get(Context::THROWN_OBJECT_INDEX)->Print(os);
      break;

    case ScopeIterator::ScopeTypeClosure:
      os << "Closure:\n";
      CurrentContext()->Print(os);
      if (CurrentContext()->has_extension()) {
        Handle<HeapObject> extension(CurrentContext()->extension(), isolate_);
        if (extension->IsJSContextExtensionObject()) {
          extension->Print(os);
        }
      }
      break;

    case ScopeIterator::ScopeTypeScript:
      os << "Script:\n";
      CurrentContext()
          ->global_object()
          ->native_context()
          ->script_context_table()
          ->Print(os);
      break;

    default:
      UNREACHABLE();
  }
  PrintF("\n");
}
#endif

int ScopeIterator::GetSourcePosition() {
  if (frame_inspector_) {
    return frame_inspector_->GetSourcePosition();
  } else {
    DCHECK(!generator_.is_null());
    return generator_->source_position();
  }
}

void ScopeIterator::RetrieveScopeChain(DeclarationScope* scope) {
  DCHECK_NOT_NULL(scope);
  GetNestedScopeChain(isolate_, scope, GetSourcePosition());
}

void ScopeIterator::CollectNonLocals(ParseInfo* info, DeclarationScope* scope) {
  DCHECK_NOT_NULL(scope);
  DCHECK(non_locals_.is_null());
  non_locals_ = scope->CollectNonLocals(info, StringSet::New(isolate_));
}


MaybeHandle<JSObject> ScopeIterator::MaterializeScriptScope() {
  Handle<JSGlobalObject> global(CurrentContext()->global_object());
  Handle<ScriptContextTable> script_contexts(
      global->native_context()->script_context_table());

  Handle<JSObject> script_scope =
      isolate_->factory()->NewJSObjectWithNullProto();

  for (int context_index = 0; context_index < script_contexts->used();
       context_index++) {
    Handle<Context> context =
        ScriptContextTable::GetContext(script_contexts, context_index);
    Handle<ScopeInfo> scope_info(context->scope_info());
    CopyContextLocalsToScopeObject(scope_info, context, script_scope);
  }
  return script_scope;
}

void ScopeIterator::MaterializeStackLocals(Handle<JSObject> local_scope,
                                           Handle<ScopeInfo> scope_info) {
  if (frame_inspector_) {
    return frame_inspector_->MaterializeStackLocals(local_scope, scope_info);
  }

  DCHECK(!generator_.is_null());
  // Fill all stack locals.
  Handle<FixedArray> parameters_and_registers(
      generator_->parameters_and_registers());
  int parameter_count = scope_info->ParameterCount();
  for (int i = 0; i < parameter_count; ++i) {
    // Do not materialize the parameter if it is shadowed by a context local.
    // TODO(yangguo): check whether this is necessary, now that we materialize
    //                context locals as well.
    Handle<String> name(scope_info->ParameterName(i));
    if (ScopeInfo::VariableIsSynthetic(*name)) continue;

    // Skip the parameter if it is context-allocated.
    VariableMode mode;
    InitializationFlag init_flag;
    MaybeAssignedFlag maybe_assigned_flag;
    if (ScopeInfo::ContextSlotIndex(scope_info, name, &mode, &init_flag,
                                    &maybe_assigned_flag) != -1) {
      continue;
    }

    Handle<Object> value(parameters_and_registers->get(i), isolate_);
    DCHECK(!value.is_identical_to(isolate_->factory()->stale_register()));

    // TODO(yangguo): We convert optimized out values to {undefined} when they
    // are passed to the debugger. Eventually we should handle them somehow.
    if (value->IsTheHole(isolate_) || value->IsOptimizedOut(isolate_)) {
      value = isolate_->factory()->undefined_value();
    }

    JSObject::SetOwnPropertyIgnoreAttributes(local_scope, name, value, NONE)
        .Check();
  }

  for (int i = 0; i < scope_info->StackLocalCount(); ++i) {
    Handle<String> name = handle(scope_info->StackLocalName(i));
    if (ScopeInfo::VariableIsSynthetic(*name)) continue;
    Handle<Object> value(parameters_and_registers->get(
                             parameter_count + scope_info->StackLocalIndex(i)),
                         isolate_);
    DCHECK(!value.is_identical_to(isolate_->factory()->stale_register()));

    // TODO(yangguo): We convert optimized out values to {undefined} when they
    // are passed to the debugger. Eventually we should handle them somehow.
    if (value->IsTheHole(isolate_) || value->IsOptimizedOut(isolate_)) {
      value = isolate_->factory()->undefined_value();
    }
    JSObject::SetOwnPropertyIgnoreAttributes(local_scope, name, value, NONE)
        .Check();
  }
}

MaybeHandle<JSObject> ScopeIterator::MaterializeLocalScope() {
  DCHECK(HasNestedScopeChain());
  Handle<SharedFunctionInfo> shared(function_->shared());
  Handle<ScopeInfo> scope_info(shared->scope_info());

  Handle<JSObject> local_scope =
      isolate_->factory()->NewJSObjectWithNullProto();
  MaterializeStackLocals(local_scope, scope_info);

  if (!scope_info->HasContext()) return local_scope;

  // Fill all context locals.
  Handle<Context> function_context(context_->closure_context());
  DCHECK_EQ(context_->scope_info(), *scope_info);
  CopyContextLocalsToScopeObject(scope_info, function_context, local_scope);

  // Finally copy any properties from the function context extension.
  // These will be variables introduced by eval.
  if (!function_context->IsNativeContext()) {
    CopyContextExtensionToScopeObject(function_context, local_scope,
                                      KeyCollectionMode::kIncludePrototypes);
  }

  return local_scope;
}


// Create a plain JSObject which materializes the closure content for the
// context.
Handle<JSObject> ScopeIterator::MaterializeClosure() {
  Handle<Context> context = CurrentContext();
  DCHECK(context->IsFunctionContext() || context->IsEvalContext());

  Handle<ScopeInfo> scope_info(context_->scope_info());

  // Allocate and initialize a JSObject with all the content of this function
  // closure.
  Handle<JSObject> closure_scope =
      isolate_->factory()->NewJSObjectWithNullProto();

  // Fill all context locals to the context extension.
  CopyContextLocalsToScopeObject(scope_info, context, closure_scope);

  // Finally copy any properties from the function context extension. This will
  // be variables introduced by eval.
  CopyContextExtensionToScopeObject(context, closure_scope,
                                    KeyCollectionMode::kOwnOnly);
  return closure_scope;
}


// Retrieve the with-context extension object. If the extension object is
// a proxy, return an empty object.
Handle<JSObject> ScopeIterator::WithContextExtension() {
  Handle<Context> context = CurrentContext();
  DCHECK(context->IsWithContext());
  if (context->extension_receiver()->IsJSProxy()) {
    return isolate_->factory()->NewJSObjectWithNullProto();
  }
  return handle(JSObject::cast(context->extension_receiver()));
}

// Create a plain JSObject which materializes the block scope for the specified
// block context.
Handle<JSObject> ScopeIterator::MaterializeInnerScope() {
  Handle<JSObject> inner_scope =
      isolate_->factory()->NewJSObjectWithNullProto();

  Handle<Context> context = Handle<Context>::null();
  if (HasNestedScopeChain()) {
    Handle<ScopeInfo> scope_info = LastNestedScopeChain().scope_info;
    MaterializeStackLocals(inner_scope, scope_info);
    if (scope_info->HasContext()) context = CurrentContext();
  } else {
    context = CurrentContext();
  }

  if (!context.is_null()) {
    // Fill all context locals.
    CopyContextLocalsToScopeObject(CurrentScopeInfo(), context, inner_scope);
    CopyContextExtensionToScopeObject(context, inner_scope,
                                      KeyCollectionMode::kOwnOnly);
  }
  return inner_scope;
}


// Create a plain JSObject which materializes the module scope for the specified
// module context.
MaybeHandle<JSObject> ScopeIterator::MaterializeModuleScope() {
  Handle<Context> context = CurrentContext();
  DCHECK(context->IsModuleContext());
  Handle<ScopeInfo> scope_info(context->scope_info());
  Handle<JSObject> module_scope =
      isolate_->factory()->NewJSObjectWithNullProto();
  CopyContextLocalsToScopeObject(scope_info, context, module_scope);
  CopyModuleVarsToScopeObject(scope_info, context, module_scope);
  return module_scope;
}

bool ScopeIterator::SetParameterValue(Handle<ScopeInfo> scope_info,
                                      Handle<String> parameter_name,
                                      Handle<Object> new_value) {
  // Setting stack locals of optimized frames is not supported.
  HandleScope scope(isolate_);
  for (int i = 0; i < scope_info->ParameterCount(); ++i) {
    if (String::Equals(handle(scope_info->ParameterName(i)), parameter_name)) {
      // Suspended generators should not get here because all parameters should
      // be context-allocated.
      DCHECK_NOT_NULL(frame_inspector_);
      JavaScriptFrame* frame = GetFrame();
      if (frame->is_optimized()) {
        return false;
      }
      frame->SetParameterValue(i, *new_value);
      return true;
    }
  }
  return false;
}

bool ScopeIterator::SetStackVariableValue(Handle<ScopeInfo> scope_info,
                                          Handle<String> variable_name,
                                          Handle<Object> new_value) {
  // Setting stack locals of optimized frames is not supported. Suspended
  // generators are supported.
  HandleScope scope(isolate_);
  for (int i = 0; i < scope_info->StackLocalCount(); ++i) {
    if (String::Equals(handle(scope_info->StackLocalName(i)), variable_name)) {
      int stack_local_index = scope_info->StackLocalIndex(i);
      if (frame_inspector_ != nullptr) {
        // Set the variable on the stack.
        JavaScriptFrame* frame = GetFrame();
        if (frame->is_optimized()) return false;
        frame->SetExpression(stack_local_index, *new_value);
      } else {
        // Set the variable in the suspended generator.
        DCHECK(!generator_.is_null());
        Handle<FixedArray> parameters_and_registers(
            generator_->parameters_and_registers());
        int index_in_parameters_and_registers =
            scope_info->ParameterCount() + stack_local_index;
        parameters_and_registers->set(index_in_parameters_and_registers,
                                      *new_value);
      }
      return true;
    }
  }
  return false;
}

bool ScopeIterator::SetContextVariableValue(Handle<ScopeInfo> scope_info,
                                            Handle<Context> context,
                                            Handle<String> variable_name,
                                            Handle<Object> new_value) {
  HandleScope scope(isolate_);
  for (int i = 0; i < scope_info->ContextLocalCount(); i++) {
    Handle<String> next_name(scope_info->ContextLocalName(i));
    if (String::Equals(variable_name, next_name)) {
      VariableMode mode;
      InitializationFlag init_flag;
      MaybeAssignedFlag maybe_assigned_flag;
      int context_index = ScopeInfo::ContextSlotIndex(
          scope_info, next_name, &mode, &init_flag, &maybe_assigned_flag);
      context->set(context_index, *new_value);
      return true;
    }
  }

  // TODO(neis): Clean up context "extension" mess.
  if (!context->IsModuleContext() && context->has_extension()) {
    Handle<JSObject> ext(context->extension_object());
    Maybe<bool> maybe = JSReceiver::HasOwnProperty(ext, variable_name);
    DCHECK(maybe.IsJust());
    if (maybe.FromJust()) {
      // We don't expect this to do anything except replacing property value.
      JSObject::SetOwnPropertyIgnoreAttributes(ext, variable_name, new_value,
                                               NONE)
          .Check();
      return true;
    }
  }

  return false;
}

bool ScopeIterator::SetLocalVariableValue(Handle<String> variable_name,
                                          Handle<Object> new_value) {
  Handle<ScopeInfo> scope_info;
  if (HasNestedScopeChain()) {
    scope_info = handle(function_->shared()->scope_info());
    DCHECK_IMPLIES(scope_info->HasContext(),
                   context_->scope_info() == *scope_info);
  } else {
    scope_info = handle(context_->scope_info());
  }

  // Parameter might be shadowed in context. Don't stop here.
  bool result = SetParameterValue(scope_info, variable_name, new_value);

  // Stack locals.
  if (SetStackVariableValue(scope_info, variable_name, new_value)) {
    return true;
  }

  if (scope_info->HasContext() &&
      SetContextVariableValue(scope_info, context_, variable_name, new_value)) {
    return true;
  }

  return result;
}

bool ScopeIterator::SetModuleVariableValue(Handle<String> variable_name,
                                           Handle<Object> new_value) {
  DCHECK_NOT_NULL(frame_inspector_);

  // Get module context and its scope info.
  Handle<Context> context = CurrentContext();
  while (!context->IsModuleContext()) {
    context = handle(context->previous(), isolate_);
  }
  Handle<ScopeInfo> scope_info(context->scope_info(), isolate_);
  DCHECK_EQ(scope_info->scope_type(), MODULE_SCOPE);

  if (SetContextVariableValue(scope_info, context, variable_name, new_value)) {
    return true;
  }

  int cell_index;
  {
    VariableMode mode;
    InitializationFlag init_flag;
    MaybeAssignedFlag maybe_assigned_flag;
    cell_index = scope_info->ModuleIndex(variable_name, &mode, &init_flag,
                                         &maybe_assigned_flag);
  }

  // Setting imports is currently not supported.
  bool found = ModuleDescriptor::GetCellIndexKind(cell_index) ==
               ModuleDescriptor::kExport;
  if (found) {
    Module::StoreVariable(handle(context->module(), isolate_), cell_index,
                          new_value);
  }
  return found;
}

bool ScopeIterator::SetInnerScopeVariableValue(Handle<String> variable_name,
                                               Handle<Object> new_value) {
  Handle<ScopeInfo> scope_info = CurrentScopeInfo();
  DCHECK(scope_info->scope_type() == BLOCK_SCOPE ||
         scope_info->scope_type() == EVAL_SCOPE ||
         scope_info->scope_type() == CATCH_SCOPE);

  // Setting stack locals of optimized frames is not supported.
  if (SetStackVariableValue(scope_info, variable_name, new_value)) {
    return true;
  }

  if (HasContext() && SetContextVariableValue(scope_info, CurrentContext(),
                                              variable_name, new_value)) {
    return true;
  }

  return false;
}

// This method copies structure of MaterializeClosure method above.
bool ScopeIterator::SetClosureVariableValue(Handle<String> variable_name,
                                            Handle<Object> new_value) {
  DCHECK(CurrentContext()->IsFunctionContext() ||
         CurrentContext()->IsEvalContext());
  return SetContextVariableValue(CurrentScopeInfo(), CurrentContext(),
                                 variable_name, new_value);
}

bool ScopeIterator::SetScriptVariableValue(Handle<String> variable_name,
                                           Handle<Object> new_value) {
  Handle<String> internalized_variable_name =
      isolate_->factory()->InternalizeString(variable_name);
  Handle<Context> context = CurrentContext();
  Handle<ScriptContextTable> script_contexts(
      context->global_object()->native_context()->script_context_table());
  ScriptContextTable::LookupResult lookup_result;
  if (ScriptContextTable::Lookup(script_contexts, internalized_variable_name,
                                 &lookup_result)) {
    Handle<Context> script_context = ScriptContextTable::GetContext(
        script_contexts, lookup_result.context_index);
    script_context->set(lookup_result.slot_index, *new_value);
    return true;
  }

  return false;
}

void ScopeIterator::CopyContextLocalsToScopeObject(
    Handle<ScopeInfo> scope_info, Handle<Context> context,
    Handle<JSObject> scope_object) {
  Isolate* isolate = scope_info->GetIsolate();
  int local_count = scope_info->ContextLocalCount();
  if (local_count == 0) return;
  // Fill all context locals to the context extension.
  for (int i = 0; i < local_count; ++i) {
    Handle<String> name(scope_info->ContextLocalName(i));
    if (ScopeInfo::VariableIsSynthetic(*name)) continue;
    int context_index = Context::MIN_CONTEXT_SLOTS + i;
    Handle<Object> value = Handle<Object>(context->get(context_index), isolate);
    // Reflect variables under TDZ as undefined in scope object.
    if (value->IsTheHole(isolate)) continue;
    // This should always succeed.
    // TODO(verwaest): Use AddDataProperty instead.
    JSObject::SetOwnPropertyIgnoreAttributes(scope_object, name, value, NONE)
        .Check();
  }
}

void ScopeIterator::CopyModuleVarsToScopeObject(Handle<ScopeInfo> scope_info,
                                                Handle<Context> context,
                                                Handle<JSObject> scope_object) {
  Isolate* isolate = scope_info->GetIsolate();

  int module_variable_count =
      Smi::cast(scope_info->get(scope_info->ModuleVariableCountIndex()))
          ->value();
  for (int i = 0; i < module_variable_count; ++i) {
    Handle<String> local_name;
    Handle<Object> value;
    {
      String* name;
      int index;
      scope_info->ModuleVariable(i, &name, &index);
      CHECK(!ScopeInfo::VariableIsSynthetic(name));
      local_name = handle(name, isolate);
      value = Module::LoadVariable(handle(context->module(), isolate), index);
    }

    // Reflect variables under TDZ as undefined in scope object.
    if (value->IsTheHole(isolate)) continue;
    // This should always succeed.
    // TODO(verwaest): Use AddDataProperty instead.
    JSObject::SetOwnPropertyIgnoreAttributes(scope_object, local_name, value,
                                             NONE)
        .Check();
  }
}

void ScopeIterator::CopyContextExtensionToScopeObject(
    Handle<Context> context, Handle<JSObject> scope_object,
    KeyCollectionMode mode) {
  if (context->extension_object() == nullptr) return;
  Handle<JSObject> extension(context->extension_object());
  Handle<FixedArray> keys =
      KeyAccumulator::GetKeys(extension, mode, ENUMERABLE_STRINGS)
          .ToHandleChecked();

  for (int i = 0; i < keys->length(); i++) {
    // Names of variables introduced by eval are strings.
    DCHECK(keys->get(i)->IsString());
    Handle<String> key(String::cast(keys->get(i)));
    Handle<Object> value =
        Object::GetPropertyOrElement(extension, key).ToHandleChecked();
    JSObject::SetOwnPropertyIgnoreAttributes(scope_object, key, value, NONE)
        .Check();
  }
}

void ScopeIterator::GetNestedScopeChain(Isolate* isolate, Scope* scope,
                                        int position) {
  if (scope->is_function_scope()) {
    // Do not collect scopes of nested inner functions inside the current one.
    // Nested arrow functions could have the same end positions.
    Handle<JSFunction> function = GetFunction();
    if (scope->start_position() > function->shared()->StartPosition() &&
        scope->end_position() <= function->shared()->EndPosition()) {
      return;
    }
  }
  if (scope->is_hidden()) {
    // We need to add this chain element in case the scope has a context
    // associated. We need to keep the scope chain and context chain in sync.
    nested_scope_chain_.emplace_back(scope->scope_info());
  } else {
    nested_scope_chain_.emplace_back(
        scope->scope_info(), scope->start_position(), scope->end_position());
  }
  for (Scope* inner_scope = scope->inner_scope(); inner_scope != nullptr;
       inner_scope = inner_scope->sibling()) {
    int beg_pos = inner_scope->start_position();
    int end_pos = inner_scope->end_position();
    DCHECK((beg_pos >= 0 && end_pos >= 0) || inner_scope->is_hidden());
    if (beg_pos <= position && position < end_pos) {
      GetNestedScopeChain(isolate, inner_scope, position);
      return;
    }
  }
}

bool ScopeIterator::HasNestedScopeChain() const {
  return !nested_scope_chain_.empty();
}

ScopeIterator::ExtendedScopeInfo& ScopeIterator::LastNestedScopeChain() {
  DCHECK(HasNestedScopeChain());
  return nested_scope_chain_.back();
}

}  // namespace internal
}  // namespace v8
