// Copyright 2020 the V8 project authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "src/heap/cppgc-js/cpp-snapshot.h"

#include <memory>

#include "include/cppgc/internal/name-trait.h"
#include "include/cppgc/trace-trait.h"
#include "include/v8-cppgc.h"
#include "include/v8-profiler.h"
#include "src/api/api-inl.h"
#include "src/base/logging.h"
#include "src/execution/isolate.h"
#include "src/heap/cppgc-js/cpp-heap.h"
#include "src/heap/cppgc-js/wrappable-info-inl.h"
#include "src/heap/cppgc-js/wrappable-info.h"
#include "src/heap/cppgc/heap-object-header.h"
#include "src/heap/cppgc/heap-visitor.h"
#include "src/heap/cppgc/visitor.h"
#include "src/heap/embedder-tracing.h"
#include "src/heap/mark-compact.h"
#include "src/objects/js-objects.h"
#include "src/profiler/heap-profiler.h"

namespace v8 {
namespace internal {

class CppGraphBuilderImpl;
class StateStorage;
class State;

using cppgc::internal::GCInfo;
using cppgc::internal::GlobalGCInfoTable;
using cppgc::internal::HeapObjectHeader;

// Node representing a C++ object on the heap.
class EmbedderNode : public v8::EmbedderGraph::Node {
 public:
  EmbedderNode(cppgc::internal::HeapObjectName name, size_t size)
      : name_(name), size_(size) {
    USE(size_);
  }
  ~EmbedderNode() override = default;

  const char* Name() final { return name_.value; }
  size_t SizeInBytes() final { return name_.name_was_hidden ? 0 : size_; }

  void SetWrapperNode(v8::EmbedderGraph::Node* wrapper_node) {
    // An embedder node may only be merged with a single wrapper node, as
    // consumers of the graph may merge a node and its wrapper node.
    //
    // TODO(chromium:1218404): Add a DCHECK() to avoid overriding an already
    // set `wrapper_node_`. This can currently happen with global proxies that
    // are rewired (and still kept alive) after reloading a page, see
    // `AddEdge`. We accept overriding the wrapper node in such cases,
    // leading to a random merged node and separated nodes for all other
    // proxies.
    wrapper_node_ = wrapper_node;
  }
  Node* WrapperNode() final { return wrapper_node_; }

  void SetDetachedness(Detachedness detachedness) {
    detachedness_ = detachedness;
  }
  Detachedness GetDetachedness() final { return detachedness_; }

  // Edge names are passed to V8 but are required to be held alive from the
  // embedder until the snapshot is compiled.
  const char* InternalizeEdgeName(std::string edge_name) {
    const size_t edge_name_len = edge_name.length();
    named_edges_.emplace_back(std::make_unique<char[]>(edge_name_len + 1));
    char* named_edge_str = named_edges_.back().get();
    snprintf(named_edge_str, edge_name_len + 1, "%s", edge_name.c_str());
    return named_edge_str;
  }

 private:
  cppgc::internal::HeapObjectName name_;
  size_t size_;
  Node* wrapper_node_ = nullptr;
  Detachedness detachedness_ = Detachedness::kUnknown;
  std::vector<std::unique_ptr<char[]>> named_edges_;
};

// Node representing an artificial root group, e.g., set of Persistent handles.
class EmbedderRootNode final : public EmbedderNode {
 public:
  explicit EmbedderRootNode(const char* name)
      : EmbedderNode({name, false}, 0) {}
  ~EmbedderRootNode() final = default;

  bool IsRootNode() final { return true; }
};

// Canonical state representing real and artificial (e.g. root) objects.
class StateBase {
 public:
  // Objects can either be hidden/visible, or depend on some other nodes while
  // traversing the same SCC.
  enum class Visibility {
    kHidden,
    kDependentVisibility,
    kVisible,
  };

  StateBase(const void* key, size_t state_count, Visibility visibility,
            EmbedderNode* node, bool visited)
      : key_(key),
        state_count_(state_count),
        visibility_(visibility),
        node_(node),
        visited_(visited) {
    DCHECK_NE(Visibility::kDependentVisibility, visibility);
  }
  virtual ~StateBase() = default;

  // Visited objects have already been processed or are currently being
  // processed, see also IsPending() below.
  bool IsVisited() const { return visited_; }

  // Pending objects are currently being processed as part of the same SCC.
  bool IsPending() const { return pending_; }

  bool IsVisibleNotDependent() {
    auto v = GetVisibility();
    CHECK_NE(Visibility::kDependentVisibility, v);
    return v == Visibility::kVisible;
  }

  void set_node(EmbedderNode* node) {
    CHECK_EQ(Visibility::kVisible, GetVisibility());
    DCHECK_NULL(node_);
    node_ = node;
  }

  EmbedderNode* get_node() {
    CHECK_EQ(Visibility::kVisible, GetVisibility());
    return node_;
  }

 protected:
  const void* key_;
  // State count keeps track of node processing order. It is used to create only
  // dependencies on ancestors in the sub graph which ensures that there will be
  // no cycles in dependencies.
  const size_t state_count_;

  Visibility visibility_;
  StateBase* visibility_dependency_ = nullptr;
  EmbedderNode* node_;
  bool visited_;
  bool pending_ = false;

  Visibility GetVisibility() {
    FollowDependencies();
    return visibility_;
  }

  StateBase* FollowDependencies() {
    if (visibility_ != Visibility::kDependentVisibility) {
      CHECK_NULL(visibility_dependency_);
      return this;
    }
    StateBase* current = this;
    std::vector<StateBase*> dependencies;
    while (current->visibility_dependency_ &&
           current->visibility_dependency_ != current) {
      DCHECK_EQ(Visibility::kDependentVisibility, current->visibility_);
      dependencies.push_back(current);
      current = current->visibility_dependency_;
    }
    auto new_visibility = Visibility::kDependentVisibility;
    auto* new_visibility_dependency = current;
    if (current->visibility_ == Visibility::kVisible) {
      new_visibility = Visibility::kVisible;
      new_visibility_dependency = nullptr;
    } else if (!IsPending()) {
      DCHECK(IsVisited());
      // The object was not visible (above case). Having a dependency on itself
      // or null means no visible object was found.
      new_visibility = Visibility::kHidden;
      new_visibility_dependency = nullptr;
    }
    current->visibility_ = new_visibility;
    current->visibility_dependency_ = new_visibility_dependency;
    for (auto* state : dependencies) {
      state->visibility_ = new_visibility;
      state->visibility_dependency_ = new_visibility_dependency;
    }
    return current;
  }

  friend class State;
};

class State final : public StateBase {
 public:
  State(const HeapObjectHeader& header, size_t state_count)
      : StateBase(&header, state_count, Visibility::kHidden, nullptr, false) {}
  ~State() final = default;

  const HeapObjectHeader* header() const {
    return static_cast<const HeapObjectHeader*>(key_);
  }

  void MarkVisited() { visited_ = true; }

  void MarkPending() { pending_ = true; }
  void UnmarkPending() { pending_ = false; }

  void MarkVisible() {
    visibility_ = Visibility::kVisible;
    visibility_dependency_ = nullptr;
  }

  void MarkDependentVisibility(StateBase* dependency) {
    // Follow and update dependencies as much as possible.
    dependency = dependency->FollowDependencies();
    DCHECK(dependency->IsVisited());
    if (visibility_ == StateBase::Visibility::kVisible) {
      // Already visible, no dependency needed.
      DCHECK_NULL(visibility_dependency_);
      return;
    }
    if (dependency->visibility_ == Visibility::kVisible) {
      // Simple case: Dependency is visible.
      visibility_ = Visibility::kVisible;
      visibility_dependency_ = nullptr;
      return;
    }
    if ((visibility_dependency_ &&
         (visibility_dependency_->state_count_ > dependency->state_count_)) ||
        (!visibility_dependency_ &&
         (state_count_ > dependency->state_count_))) {
      // Only update when new state_count_ < original state_count_. This
      // ensures that we pick an ancestor as dependency and not a child which
      // is guaranteed to converge to an answer.
      //
      // Dependency is now
      // a) either pending with unknown visibility (same call chain), or
      // b) not pending and has defined visibility.
      //
      // It's not possible to point to a state that is not pending but has
      // dependent visibility because dependencies are updated to the top-most
      // dependency at the beginning of method.
      if (dependency->IsPending()) {
        visibility_ = Visibility::kDependentVisibility;
        visibility_dependency_ = dependency;
      } else {
        CHECK_NE(Visibility::kDependentVisibility, dependency->visibility_);
        if (dependency->visibility_ == Visibility::kVisible) {
          visibility_ = Visibility::kVisible;
          visibility_dependency_ = nullptr;
        }
      }
    }
  }

  void MarkAsWeakContainer() { is_weak_container_ = true; }
  bool IsWeakContainer() const { return is_weak_container_; }

  void AddEphemeronEdge(const HeapObjectHeader& value) {
    // This ignores duplicate entries (in different containers) for the same
    // Key->Value pairs. Only one edge will be emitted in this case.
    ephemeron_edges_.insert(&value);
  }

  void AddEagerEphemeronEdge(const void* value, cppgc::TraceCallback callback) {
    eager_ephemeron_edges_.insert({value, callback});
  }

  template <typename Callback>
  void ForAllEphemeronEdges(Callback callback) {
    for (const HeapObjectHeader* value : ephemeron_edges_) {
      callback(*value);
    }
  }

  template <typename Callback>
  void ForAllEagerEphemeronEdges(Callback callback) {
    for (const auto& pair : eager_ephemeron_edges_) {
      callback(pair.first, pair.second);
    }
  }

 private:
  bool is_weak_container_ = false;
  // Values that are held alive through ephemerons by this particular key.
  std::unordered_set<const HeapObjectHeader*> ephemeron_edges_;
  // Values that are eagerly traced and held alive through ephemerons by this
  // particular key.
  std::unordered_map<const void*, cppgc::TraceCallback> eager_ephemeron_edges_;
};

// Root states are similar to regular states with the difference that they are
// always visible.
class RootState final : public StateBase {
 public:
  RootState(EmbedderRootNode* node, size_t state_count)
      // Root states are always visited, visible, and have a node attached.
      : StateBase(node, state_count, Visibility::kVisible, node, true) {}
  ~RootState() final = default;
};

// Abstraction for storing states. Storage allows for creation and lookup of
// different state objects.
class StateStorage final {
 public:
  bool StateExists(const void* key) const {
    return states_.find(key) != states_.end();
  }

  StateBase& GetExistingState(const void* key) const {
    CHECK(StateExists(key));
    return *states_.at(key).get();
  }

  State& GetExistingState(const HeapObjectHeader& header) const {
    return static_cast<State&>(GetExistingState(&header));
  }

  State& GetOrCreateState(const HeapObjectHeader& header) {
    if (!StateExists(&header)) {
      auto it = states_.insert(std::make_pair(
          &header, std::make_unique<State>(header, ++state_count_)));
      DCHECK(it.second);
      USE(it);
    }
    return GetExistingState(header);
  }

  RootState& CreateRootState(EmbedderRootNode* root_node) {
    CHECK(!StateExists(root_node));
    auto it = states_.insert(std::make_pair(
        root_node, std::make_unique<RootState>(root_node, ++state_count_)));
    DCHECK(it.second);
    USE(it);
    return static_cast<RootState&>(*it.first->second.get());
  }

  template <typename Callback>
  void ForAllVisibleStates(Callback callback) {
    for (auto& state : states_) {
      if (state.second->IsVisibleNotDependent()) {
        callback(state.second.get());
      }
    }
  }

 private:
  std::unordered_map<const void*, std::unique_ptr<StateBase>> states_;
  size_t state_count_ = 0;
};

void* ExtractEmbedderDataBackref(Isolate* isolate, CppHeap& cpp_heap,
                                 v8::Local<v8::Value> v8_value) {
  // See LocalEmbedderHeapTracer::VerboseWrapperTypeInfo for details on how
  // wrapper objects are set up.
  if (!v8_value->IsObject()) return nullptr;

  Handle<Object> v8_object = Utils::OpenHandle(*v8_value);
  if (!v8_object->IsJSObject() ||
      !JSObject::cast(*v8_object).MayHaveEmbedderFields())
    return nullptr;

  JSObject js_object = JSObject::cast(*v8_object);

  const auto maybe_info =
      WrappableInfo::From(isolate, js_object, cpp_heap.wrapper_descriptor());
  return maybe_info.has_value() ? maybe_info->instance : nullptr;
}

// The following implements a snapshotting algorithm for C++ objects that also
// filters strongly-connected components (SCCs) of only "hidden" objects that
// are not (transitively) referencing any non-hidden objects.
//
// C++ objects come in two versions.
// a. Named objects that have been assigned a name through NameProvider.
// b. Unnamed objects, that are potentially hidden if the build configuration
//    requires Oilpan to hide such names. Hidden objects have their name
//    set to NameProvider::kHiddenName.
//
// The main challenge for the algorithm is to avoid blowing up the final object
// graph with hidden nodes that do not carry information. For that reason, the
// algorithm filters SCCs of only hidden objects, e.g.:
//   ... -> (object) -> (object) -> (hidden) -> (hidden)
// In this case the (hidden) objects are filtered from the graph. The trickiest
// part is maintaining visibility state for objects referencing other objects
// that are currently being processed.
//
// Main algorithm idea (two passes):
// 1. First pass marks all non-hidden objects and those that transitively reach
//    non-hidden objects as visible. Details:
//    - Iterate over all objects.
//    - If object is non-hidden mark it as visible and also mark parent as
//      visible if needed.
//    - If object is hidden, traverse children as DFS to find non-hidden
//      objects. Post-order process the objects and mark those objects as
//      visible that have child nodes that are visible themselves.
//    - Maintain an epoch counter (StateStorage::state_count_) to allow
//      deferring the visibility decision to other objects in the same SCC. This
//      is similar to the "lowlink" value in Tarjan's algorithm for SCC.
//    - After the first pass it is guaranteed that all deferred visibility
//      decisions can be resolved.
// 2. Second pass adds nodes and edges for all visible objects.
//    - Upon first checking the visibility state of an object, all deferred
//      visibility states are resolved.
//
// For practical reasons, the recursion is transformed into an iteration. We do
// do not use plain Tarjan's algorithm to avoid another pass over all nodes to
// create SCCs.
class CppGraphBuilderImpl final {
 public:
  CppGraphBuilderImpl(CppHeap& cpp_heap, v8::EmbedderGraph& graph)
      : cpp_heap_(cpp_heap), graph_(graph) {}

  void Run();

  void VisitForVisibility(State* parent, const HeapObjectHeader&);
  void VisitForVisibility(State& parent, const TracedReferenceBase&);
  void VisitEphemeronForVisibility(const HeapObjectHeader& key,
                                   const HeapObjectHeader& value);
  void VisitEphemeronWithNonGarbageCollectedValueForVisibility(
      const HeapObjectHeader& key, const void* value,
      cppgc::TraceDescriptor value_desc);
  void VisitWeakContainerForVisibility(const HeapObjectHeader&);
  void VisitRootForGraphBuilding(RootState&, const HeapObjectHeader&,
                                 const cppgc::SourceLocation&);
  void ProcessPendingObjects();

  EmbedderRootNode* AddRootNode(const char* name) {
    return static_cast<EmbedderRootNode*>(graph_.AddNode(
        std::unique_ptr<v8::EmbedderGraph::Node>{new EmbedderRootNode(name)}));
  }

  EmbedderNode* AddNode(const HeapObjectHeader& header) {
    return static_cast<EmbedderNode*>(
        graph_.AddNode(std::unique_ptr<v8::EmbedderGraph::Node>{
            new EmbedderNode(header.GetName(), header.AllocatedSize())}));
  }

  void AddEdge(State& parent, const HeapObjectHeader& header,
               const std::string& edge_name) {
    DCHECK(parent.IsVisibleNotDependent());
    auto& current = states_.GetExistingState(header);
    if (!current.IsVisibleNotDependent()) return;

    // Both states are visible. Create nodes in case this is the first edge
    // created for any of them.
    if (!parent.get_node()) {
      parent.set_node(AddNode(*parent.header()));
    }
    if (!current.get_node()) {
      current.set_node(AddNode(header));
    }

    if (!edge_name.empty()) {
      graph_.AddEdge(parent.get_node(), current.get_node(),
                     parent.get_node()->InternalizeEdgeName(edge_name));
    } else {
      graph_.AddEdge(parent.get_node(), current.get_node());
    }
  }

  void AddEdge(State& parent, const TracedReferenceBase& ref,
               const std::string& edge_name) {
    DCHECK(parent.IsVisibleNotDependent());
    v8::Local<v8::Value> v8_value =
        ref.Get(reinterpret_cast<v8::Isolate*>(cpp_heap_.isolate()));
    if (!v8_value.IsEmpty()) {
      if (!parent.get_node()) {
        parent.set_node(AddNode(*parent.header()));
      }
      auto* v8_node = graph_.V8Node(v8_value);
      if (!edge_name.empty()) {
        graph_.AddEdge(parent.get_node(), v8_node,
                       parent.get_node()->InternalizeEdgeName(edge_name));
      } else {
        graph_.AddEdge(parent.get_node(), v8_node);
      }

      // References that have a class id set may have their internal fields
      // pointing back to the object. Set up a wrapper node for the graph so
      // that the snapshot generator  can merge the nodes appropriately.
      // Even with a set class id, do not set up a wrapper node when the edge
      // has a specific name.
      if (!ref.WrapperClassId() || !edge_name.empty()) return;

      void* back_reference_object = ExtractEmbedderDataBackref(
          reinterpret_cast<v8::internal::Isolate*>(cpp_heap_.isolate()),
          cpp_heap_, v8_value);
      if (back_reference_object) {
        auto& back_header = HeapObjectHeader::FromObject(back_reference_object);
        auto& back_state = states_.GetExistingState(back_header);

        // Generally the back reference will point to `parent.header()`. In the
        // case of global proxy set up the backreference will point to a
        // different object, which may not have a node at t his point. Merge the
        // nodes nevertheless as Window objects need to be able to query their
        // detachedness state.
        //
        // TODO(chromium:1218404): See bug description on how to fix this
        // inconsistency and only merge states when the backref points back
        // to the same object.
        if (!back_state.get_node()) {
          back_state.set_node(AddNode(back_header));
        }
        back_state.get_node()->SetWrapperNode(v8_node);

        auto* profiler =
            reinterpret_cast<Isolate*>(cpp_heap_.isolate())->heap_profiler();
        if (profiler->HasGetDetachednessCallback()) {
          back_state.get_node()->SetDetachedness(
              profiler->GetDetachedness(v8_value, ref.WrapperClassId()));
        }
      }
    }
  }

  void AddRootEdge(RootState& root, State& child, std::string edge_name) {
    DCHECK(root.IsVisibleNotDependent());
    if (!child.IsVisibleNotDependent()) return;

    // Root states always have a node set.
    DCHECK_NOT_NULL(root.get_node());
    if (!child.get_node()) {
      child.set_node(AddNode(*child.header()));
    }

    if (!edge_name.empty()) {
      graph_.AddEdge(root.get_node(), child.get_node(),
                     root.get_node()->InternalizeEdgeName(edge_name));
      return;
    }
    graph_.AddEdge(root.get_node(), child.get_node());
  }

 private:
  class WorkstackItemBase;
  class VisitationItem;
  class VisitationDoneItem;

  struct MergedNodeItem {
    EmbedderGraph::Node* node_;
    v8::Local<v8::Value> value_;
    uint16_t wrapper_class_id_;
  };

  CppHeap& cpp_heap_;
  v8::EmbedderGraph& graph_;
  StateStorage states_;
  std::vector<std::unique_ptr<WorkstackItemBase>> workstack_;
};

// Iterating live objects to mark them as visible if needed.
class LiveObjectsForVisibilityIterator final
    : public cppgc::internal::HeapVisitor<LiveObjectsForVisibilityIterator> {
  friend class cppgc::internal::HeapVisitor<LiveObjectsForVisibilityIterator>;

 public:
  explicit LiveObjectsForVisibilityIterator(CppGraphBuilderImpl& graph_builder)
      : graph_builder_(graph_builder) {}

 private:
  bool VisitHeapObjectHeader(HeapObjectHeader& header) {
    if (header.IsFree()) return true;
    graph_builder_.VisitForVisibility(nullptr, header);
    graph_builder_.ProcessPendingObjects();
    return true;
  }

  CppGraphBuilderImpl& graph_builder_;
};

class ParentScope final {
 public:
  explicit ParentScope(StateBase& parent) : parent_(parent) {}

  RootState& ParentAsRootState() const {
    return static_cast<RootState&>(parent_);
  }
  State& ParentAsRegularState() const { return static_cast<State&>(parent_); }

 private:
  StateBase& parent_;
};

// This visitor can be used stand-alone to handle fully weak and ephemeron
// containers or as part of the VisibilityVisitor that recursively traverses
// the object graph.
class WeakVisitor : public JSVisitor {
 public:
  explicit WeakVisitor(CppGraphBuilderImpl& graph_builder)
      : JSVisitor(cppgc::internal::VisitorFactory::CreateKey()),
        graph_builder_(graph_builder) {}

  void VisitWeakContainer(const void* object,
                          cppgc::TraceDescriptor strong_desc,
                          cppgc::TraceDescriptor weak_desc, cppgc::WeakCallback,
                          const void*) final {
    const auto& container_header =
        HeapObjectHeader::FromObject(strong_desc.base_object_payload);

    graph_builder_.VisitWeakContainerForVisibility(container_header);

    if (!weak_desc.callback) {
      // Weak container does not contribute to liveness.
      return;
    }
    // Heap snapshot is always run after a GC so we know there are no dead
    // entries in the container.
    if (object) {
      // The container will itself be traced strongly via the regular Visit()
      // handling that iterates over all live objects. The visibility visitor
      // will thus see (because of strongly treating the container):
      // 1. the container itself;
      // 2. for each {key} in container: container->key;
      // 3. for each {key, value} in container: key->value;
      //
      // In case the visitor is used stand-alone, we trace through the container
      // here to create the same state as we would when the container is traced
      // separately.
      container_header.Trace(this);
    }
  }
  void VisitEphemeron(const void* key, const void* value,
                      cppgc::TraceDescriptor value_desc) final {
    // For ephemerons, the key retains the value.
    // Key always must be a GarbageCollected object.
    auto& key_header = HeapObjectHeader::FromObject(key);
    if (!value_desc.base_object_payload) {
      // Value does not represent an actual GarbageCollected object but rather
      // should be traced eagerly.
      graph_builder_.VisitEphemeronWithNonGarbageCollectedValueForVisibility(
          key_header, value, value_desc);
      return;
    }
    // Regular path where both key and value are GarbageCollected objects.
    graph_builder_.VisitEphemeronForVisibility(
        key_header, HeapObjectHeader::FromObject(value));
  }

 protected:
  CppGraphBuilderImpl& graph_builder_;
};

class VisiblityVisitor final : public WeakVisitor {
 public:
  VisiblityVisitor(CppGraphBuilderImpl& graph_builder,
                   const ParentScope& parent_scope)
      : WeakVisitor(graph_builder), parent_scope_(parent_scope) {}

  // C++ handling.
  void Visit(const void*, cppgc::TraceDescriptor desc) final {
    graph_builder_.VisitForVisibility(
        &parent_scope_.ParentAsRegularState(),
        HeapObjectHeader::FromObject(desc.base_object_payload));
  }

  // JS handling.
  void Visit(const TracedReferenceBase& ref) final {
    graph_builder_.VisitForVisibility(parent_scope_.ParentAsRegularState(),
                                      ref);
  }

 private:
  const ParentScope& parent_scope_;
};

class GraphBuildingRootVisitor final : public cppgc::internal::RootVisitorBase {
 public:
  GraphBuildingRootVisitor(CppGraphBuilderImpl& graph_builder,
                           const ParentScope& parent_scope)
      : graph_builder_(graph_builder), parent_scope_(parent_scope) {}

  void VisitRoot(const void*, cppgc::TraceDescriptor desc,
                 const cppgc::SourceLocation& loc) final {
    graph_builder_.VisitRootForGraphBuilding(
        parent_scope_.ParentAsRootState(),
        HeapObjectHeader::FromObject(desc.base_object_payload), loc);
  }

 private:
  CppGraphBuilderImpl& graph_builder_;
  const ParentScope& parent_scope_;
};

class GraphBuildingVisitor final : public JSVisitor {
 public:
  GraphBuildingVisitor(CppGraphBuilderImpl& graph_builder,
                       const ParentScope& parent_scope)
      : JSVisitor(cppgc::internal::VisitorFactory::CreateKey()),
        graph_builder_(graph_builder),
        parent_scope_(parent_scope) {}

  // C++ handling.
  void Visit(const void*, cppgc::TraceDescriptor desc) final {
    graph_builder_.AddEdge(
        parent_scope_.ParentAsRegularState(),
        HeapObjectHeader::FromObject(desc.base_object_payload), edge_name_);
  }
  void VisitWeakContainer(const void* object,
                          cppgc::TraceDescriptor strong_desc,
                          cppgc::TraceDescriptor weak_desc, cppgc::WeakCallback,
                          const void*) final {
    // Add an edge from the object holding the weak container to the weak
    // container itself.
    graph_builder_.AddEdge(
        parent_scope_.ParentAsRegularState(),
        HeapObjectHeader::FromObject(strong_desc.base_object_payload),
        edge_name_);
  }

  // JS handling.
  void Visit(const TracedReferenceBase& ref) final {
    graph_builder_.AddEdge(parent_scope_.ParentAsRegularState(), ref,
                           edge_name_);
  }

  void set_edge_name(std::string edge_name) {
    edge_name_ = std::move(edge_name);
  }

 private:
  CppGraphBuilderImpl& graph_builder_;
  const ParentScope& parent_scope_;
  std::string edge_name_;
};

// Base class for transforming recursion into iteration. Items are processed
// in stack fashion.
class CppGraphBuilderImpl::WorkstackItemBase {
 public:
  WorkstackItemBase(State* parent, State& current)
      : parent_(parent), current_(current) {}

  virtual ~WorkstackItemBase() = default;
  virtual void Process(CppGraphBuilderImpl&) = 0;

 protected:
  State* parent_;
  State& current_;
};

void CppGraphBuilderImpl::ProcessPendingObjects() {
  while (!workstack_.empty()) {
    std::unique_ptr<WorkstackItemBase> item = std::move(workstack_.back());
    workstack_.pop_back();
    item->Process(*this);
  }
}

// Post-order processing of an object. It's guaranteed that all children have
// been processed first.
class CppGraphBuilderImpl::VisitationDoneItem final : public WorkstackItemBase {
 public:
  VisitationDoneItem(State* parent, State& current)
      : WorkstackItemBase(parent, current) {}

  void Process(CppGraphBuilderImpl& graph_builder) final {
    CHECK(parent_);
    parent_->MarkDependentVisibility(&current_);
    current_.UnmarkPending();
  }
};

class CppGraphBuilderImpl::VisitationItem final : public WorkstackItemBase {
 public:
  VisitationItem(State* parent, State& current)
      : WorkstackItemBase(parent, current) {}

  void Process(CppGraphBuilderImpl& graph_builder) final {
    if (parent_) {
      // Re-add the same object for post-order processing. This must happen
      // lazily, as the parent's visibility depends on its children.
      graph_builder.workstack_.push_back(std::unique_ptr<WorkstackItemBase>{
          new VisitationDoneItem(parent_, current_)});
    }
    ParentScope parent_scope(current_);
    VisiblityVisitor object_visitor(graph_builder, parent_scope);
    current_.header()->Trace(&object_visitor);
    if (!parent_) {
      current_.UnmarkPending();
    }
  }
};

void CppGraphBuilderImpl::VisitForVisibility(State* parent,
                                             const HeapObjectHeader& header) {
  auto& current = states_.GetOrCreateState(header);

  if (current.IsVisited()) {
    // Avoid traversing into already visited subgraphs and just update the state
    // based on a previous result.
    if (parent) {
      parent->MarkDependentVisibility(&current);
    }
    return;
  }

  current.MarkVisited();
  if (header.GetName().name_was_hidden) {
    current.MarkPending();
    workstack_.push_back(std::unique_ptr<WorkstackItemBase>{
        new VisitationItem(parent, current)});
  } else {
    // No need to mark/unmark pending as the node is immediately processed.
    current.MarkVisible();
    // In case the names are visible, the graph is not traversed in this phase.
    // Explicitly trace one level to handle weak containers.
    WeakVisitor weak_visitor(*this);
    header.Trace(&weak_visitor);
    if (parent) {
      // Eagerly update a parent object as its visibility state is now fixed.
      parent->MarkVisible();
    }
  }
}

void CppGraphBuilderImpl::
    VisitEphemeronWithNonGarbageCollectedValueForVisibility(
        const HeapObjectHeader& key, const void* value,
        cppgc::TraceDescriptor value_desc) {
  auto& key_state = states_.GetOrCreateState(key);
  // Eagerly trace the value here, effectively marking key as visible and
  // queuing processing for all reachable values.
  ParentScope parent_scope(key_state);
  VisiblityVisitor visitor(*this, parent_scope);
  value_desc.callback(&visitor, value);
  key_state.AddEagerEphemeronEdge(value, value_desc.callback);
}

void CppGraphBuilderImpl::VisitEphemeronForVisibility(
    const HeapObjectHeader& key, const HeapObjectHeader& value) {
  auto& key_state = states_.GetOrCreateState(key);
  VisitForVisibility(&key_state, value);
  key_state.AddEphemeronEdge(value);
}

void CppGraphBuilderImpl::VisitWeakContainerForVisibility(
    const HeapObjectHeader& container_header) {
  // Mark the container here as weak container to avoid creating any
  // outgoing edges in the second phase.
  states_.GetOrCreateState(container_header).MarkAsWeakContainer();
}

void CppGraphBuilderImpl::VisitForVisibility(State& parent,
                                             const TracedReferenceBase& ref) {
  v8::Local<v8::Value> v8_value =
      ref.Get(reinterpret_cast<v8::Isolate*>(cpp_heap_.isolate()));
  if (!v8_value.IsEmpty()) {
    parent.MarkVisible();
  }
}

void CppGraphBuilderImpl::VisitRootForGraphBuilding(
    RootState& root, const HeapObjectHeader& header,
    const cppgc::SourceLocation& loc) {
  State& current = states_.GetExistingState(header);
  if (!current.IsVisibleNotDependent()) return;

  AddRootEdge(root, current, loc.ToString());
}

void CppGraphBuilderImpl::Run() {
  // Sweeping from a previous GC might still be running, in which case not all
  // pages have been returned to spaces yet.
  cpp_heap_.sweeper().FinishIfRunning();
  // First pass: Figure out which objects should be included in the graph -- see
  // class-level comment on CppGraphBuilder.
  LiveObjectsForVisibilityIterator visitor(*this);
  visitor.Traverse(cpp_heap_.raw_heap());
  // Second pass: Add graph nodes for objects that must be shown.
  states_.ForAllVisibleStates([this](StateBase* state_base) {
    // No roots have been created so far, so all StateBase objects are State.
    State& state = *static_cast<State*>(state_base);

    // Emit no edges for the contents of the weak containers. For both, fully
    // weak and ephemeron containers, the contents should be retained from
    // somewhere else.
    if (state.IsWeakContainer()) return;

    ParentScope parent_scope(state);
    GraphBuildingVisitor object_visitor(*this, parent_scope);
    state.header()->Trace(&object_visitor);
    state.ForAllEphemeronEdges([this, &state](const HeapObjectHeader& value) {
      AddEdge(state, value, "part of key -> value pair in ephemeron table");
    });
    object_visitor.set_edge_name(
        "part of key -> value pair in ephemeron table");
    state.ForAllEagerEphemeronEdges(
        [&object_visitor](const void* value, cppgc::TraceCallback callback) {
          callback(&object_visitor, value);
        });
  });
  // Add roots.
  {
    ParentScope parent_scope(states_.CreateRootState(AddRootNode("C++ roots")));
    GraphBuildingRootVisitor root_object_visitor(*this, parent_scope);
    cpp_heap_.GetStrongPersistentRegion().Iterate(root_object_visitor);
  }
  {
    ParentScope parent_scope(
        states_.CreateRootState(AddRootNode("C++ cross-thread roots")));
    GraphBuildingRootVisitor root_object_visitor(*this, parent_scope);
    cppgc::internal::PersistentRegionLock guard;
    cpp_heap_.GetStrongCrossThreadPersistentRegion().Iterate(
        root_object_visitor);
  }
}

// static
void CppGraphBuilder::Run(v8::Isolate* isolate, v8::EmbedderGraph* graph,
                          void* data) {
  CppHeap* cpp_heap = static_cast<CppHeap*>(data);
  CHECK_NOT_NULL(cpp_heap);
  CHECK_NOT_NULL(graph);
  CppGraphBuilderImpl graph_builder(*cpp_heap, *graph);
  graph_builder.Run();
}

}  // namespace internal
}  // namespace v8
