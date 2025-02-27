// Copyright 2013 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "ui/accessibility/ax_tree.h"

#include <stddef.h>

#include <numeric>
#include <set>

#include "base/auto_reset.h"
#include "base/command_line.h"
#include "base/logging.h"
#include "base/no_destructor.h"
#include "base/stl_util.h"
#include "base/strings/stringprintf.h"
#include "ui/accessibility/accessibility_switches.h"
#include "ui/accessibility/ax_enums.mojom.h"
#include "ui/accessibility/ax_language_detection.h"
#include "ui/accessibility/ax_node.h"
#include "ui/accessibility/ax_node_position.h"
#include "ui/accessibility/ax_role_properties.h"
#include "ui/accessibility/ax_table_info.h"
#include "ui/accessibility/ax_tree_observer.h"
#include "ui/gfx/transform.h"

namespace ui {

namespace {

std::string TreeToStringHelper(const AXNode* node, int indent) {
  if (!node)
    return "";

  return std::accumulate(
      node->children().cbegin(), node->children().cend(),
      std::string(2 * indent, ' ') + node->data().ToString() + "\n",
      [indent](const std::string& str, const auto* child) {
        return str + TreeToStringHelper(child, indent + 1);
      });
}

template <typename K, typename V>
bool KeyValuePairsKeysMatch(std::vector<std::pair<K, V>> pairs1,
                            std::vector<std::pair<K, V>> pairs2) {
  if (pairs1.size() != pairs2.size())
    return false;
  for (size_t i = 0; i < pairs1.size(); ++i) {
    if (pairs1[i].first != pairs2[i].first)
      return false;
  }
  return true;
}

template <typename K, typename V>
std::map<K, V> MapFromKeyValuePairs(std::vector<std::pair<K, V>> pairs) {
  std::map<K, V> result;
  for (size_t i = 0; i < pairs.size(); ++i)
    result[pairs[i].first] = pairs[i].second;
  return result;
}

// Given two vectors of <K, V> key, value pairs representing an "old" vs "new"
// state, or "before" vs "after", calls a callback function for each key that
// changed value. Note that if an attribute is removed, that will result in
// a call to the callback with the value changing from the previous value to
// |empty_value|, and similarly when an attribute is added.
template <typename K, typename V, typename F>
void CallIfAttributeValuesChanged(const std::vector<std::pair<K, V>>& pairs1,
                                  const std::vector<std::pair<K, V>>& pairs2,
                                  const V& empty_value,
                                  F callback) {
  // Fast path - if they both have the same keys in the same order.
  if (KeyValuePairsKeysMatch(pairs1, pairs2)) {
    for (size_t i = 0; i < pairs1.size(); ++i) {
      if (pairs1[i].second != pairs2[i].second)
        callback(pairs1[i].first, pairs1[i].second, pairs2[i].second);
    }
    return;
  }

  // Slower path - they don't have the same keys in the same order, so
  // check all keys against each other, using maps to prevent this from
  // becoming O(n^2) as the size grows.
  auto map1 = MapFromKeyValuePairs(pairs1);
  auto map2 = MapFromKeyValuePairs(pairs2);
  for (size_t i = 0; i < pairs1.size(); ++i) {
    const auto& new_iter = map2.find(pairs1[i].first);
    if (pairs1[i].second != empty_value && new_iter == map2.end())
      callback(pairs1[i].first, pairs1[i].second, empty_value);
  }

  for (size_t i = 0; i < pairs2.size(); ++i) {
    const auto& iter = map1.find(pairs2[i].first);
    if (iter == map1.end())
      callback(pairs2[i].first, empty_value, pairs2[i].second);
    else if (iter->second != pairs2[i].second)
      callback(pairs2[i].first, iter->second, pairs2[i].second);
  }
}

bool IsCollapsed(const AXNode* node) {
  return node && node->data().HasState(ax::mojom::State::kCollapsed);
}

}  // namespace

// This object is used to track structure changes that will occur for a specific
// AXID. This includes how many times we expect that a node with a specific AXID
// will be created and/or destroyed, and how many times a subtree rooted at AXID
// expects to be destroyed during an AXTreeUpdate.
//
// An AXTreeUpdate is a serialized representation of an atomic change to an
// AXTree. See also |AXTreeUpdate| which documents the nature and invariants
// required to atomically update the AXTree.
//
// The reason that we must track these counts, and the reason these are counts
// rather than a bool/flag is because an AXTreeUpdate may contain multiple
// AXNodeData updates for a given AXID. A common way that this occurs is when
// multiple AXTreeUpdates are merged together, combining their AXNodeData list.
// Additionally AXIDs may be reused after being removed from the tree,
// most notably when "reparenting" a node. A "reparent" occurs when an AXID is
// first destroyed from the tree then created again in the same AXTreeUpdate,
// which may also occur multiple times with merged updates.
//
// We need to accumulate these counts for 3 reasons :
//   1. To determine what structure changes *will* occur before applying
//      updates to the tree so that we can notify observers of structure changes
//      when the tree is still in a stable and unchanged state.
//   2. Capture any errors *before* applying updates to the tree structure
//      due to the order of (or lack of) AXNodeData entries in the update
//      so we can abort a bad update instead of applying it partway.
//   3. To validate that the expectations we accumulate actually match
//      updates that are applied to the tree.
//
// To reiterate the invariants that this structure is taking a dependency on
// from |AXTreeUpdate|, suppose that the next AXNodeData to be applied is
// |node|. The following invariants must hold:
// 1. Either
//   a) |node.id| is already in the tree, or
//   b) the tree is empty, and
//      |node| is the new root of the tree, and
//      |node.role| == WebAXRoleRootWebArea.
// 2. Every child id in |node.child_ids| must either be already a child
//        of this node, or a new id not previously in the tree. It is not
//        allowed to "reparent" a child to this node without first removing
//        that child from its previous parent.
// 3. When a new id appears in |node.child_ids|, the tree should create a
//        new uninitialized placeholder node for it immediately. That
//        placeholder must be updated within the same AXTreeUpdate, otherwise
//        it's a fatal error. This guarantees the tree is always complete
//        before or after an AXTreeUpdate.
struct PendingStructureChanges {
  PendingStructureChanges(const AXNode* node)
      : destroy_subtree_count(0),
        destroy_node_count(0),
        create_node_count(0),
        node_exists(!!node),
        parent_node_id((node && node->parent())
                           ? base::Optional<AXNode::AXID>{node->parent()->id()}
                           : base::nullopt),
        last_known_data(node ? &node->data() : nullptr) {}

  // Returns true if this node has any changes remaining.
  // This includes pending subtree or node destruction, and node creation.
  bool DoesNodeExpectAnyStructureChanges() const {
    return DoesNodeExpectSubtreeWillBeDestroyed() ||
           DoesNodeExpectNodeWillBeDestroyed() ||
           DoesNodeExpectNodeWillBeCreated();
  }

  // Returns true if there are any pending changes that require destroying
  // this node or its subtree.
  bool DoesNodeExpectSubtreeOrNodeWillBeDestroyed() const {
    return DoesNodeExpectSubtreeWillBeDestroyed() ||
           DoesNodeExpectNodeWillBeDestroyed();
  }

  // Returns true if the subtree rooted at this node needs to be destroyed
  // during the update, but this may not be the next action that needs to be
  // performed on the node.
  bool DoesNodeExpectSubtreeWillBeDestroyed() const {
    return destroy_subtree_count;
  }

  // Returns true if this node needs to be destroyed during the update, but this
  // may not be the next action that needs to be performed on the node.
  bool DoesNodeExpectNodeWillBeDestroyed() const { return destroy_node_count; }

  // Returns true if this node needs be created during the update, but this
  // may not be the next action that needs to be performed on the node.
  bool DoesNodeExpectNodeWillBeCreated() const { return create_node_count; }

  // Returns true if this node would exist in the tree as of the last pending
  // update that was processed, and the node has not been provided node data.
  bool DoesNodeRequireInit() const { return node_exists && !last_known_data; }

  // Keep track of the number of times the subtree rooted at this node
  // will be destroyed.
  // An example of when this count may be larger than 1 is if updates were
  // merged together. A subtree may be [created,] destroyed, created, and
  // destroyed again within the same |AXTreeUpdate|. The important takeaway here
  // is that an update may request destruction of a subtree rooted at an
  // AXID more than once, not that a specific subtree is being destroyed
  // more than once.
  int32_t destroy_subtree_count;

  // Keep track of the number of times this node will be destroyed.
  // An example of when this count may be larger than 1 is if updates were
  // merged together. A node may be [created,] destroyed, created, and destroyed
  // again within the same |AXTreeUpdate|. The important takeaway here is that
  // an AXID may request destruction more than once, not that a specific node
  // is being destroyed more than once.
  int32_t destroy_node_count;

  // Keep track of the number of times this node will be created.
  // An example of when this count may be larger than 1 is if updates were
  // merged together. A node may be [destroyed,] created, destroyed, and created
  // again within the same |AXTreeUpdate|. The important takeaway here is that
  // an AXID may request creation more than once, not that a specific node is
  // being created more than once.
  int32_t create_node_count;

  // Keep track of whether this node exists in the tree as of the last pending
  // update that was processed.
  bool node_exists;

  // Keep track of the parent id for this node as of the last pending
  // update that was processed.
  base::Optional<AXNode::AXID> parent_node_id;

  // Keep track of the last known node data for this node.
  // This will be null either when a node does not exist in the tree, or
  // when the node is new and has not been initialized with node data yet.
  // This is needed to determine what children have changed between pending
  // updates.
  const AXNodeData* last_known_data;
};

// Represents the different states when computing PendingStructureChanges
// required for tree Unserialize.
enum class AXTreePendingStructureStatus {
  // PendingStructureChanges have not begun computation.
  kNotStarted,
  // PendingStructureChanges are currently being computed.
  kComputing,
  // All PendingStructureChanges have successfully been computed.
  kComplete,
  // An error occurred when computing pending changes.
  kFailed,
};

// Intermediate state to keep track of during a tree update.
struct AXTreeUpdateState {
  AXTreeUpdateState(const AXTree& tree)
      : pending_update_status(AXTreePendingStructureStatus::kNotStarted),
        root_will_be_created(false),
        tree(tree) {}

  // Returns whether this update removes |node|.
  bool IsRemovedNode(const AXNode* node) const {
    return base::Contains(removed_node_ids, node->id());
  }

  // Returns whether this update creates a node marked by |node_id|.
  bool IsCreatedNode(AXNode::AXID node_id) const {
    return base::Contains(new_node_ids, node_id);
  }

  // Returns whether this update creates |node|.
  bool IsCreatedNode(const AXNode* node) const {
    return IsCreatedNode(node->id());
  }

  // Returns whether this update reparents |node|.
  bool IsReparentedNode(const AXNode* node) const {
    DCHECK_EQ(AXTreePendingStructureStatus::kComplete, pending_update_status)
        << "This method should not be called before pending changes have "
           "finished computing.";
    PendingStructureChanges* data = GetPendingStructureChanges(node->id());
    if (!data)
      return false;
    // In order to know if the node will be reparented during the update,
    // we check if either the node will be destroyed or has been destroyed at
    // least once during the update.
    // Since this method is only allowed to be called after calculating all
    // pending structure changes, |node_exists| tells us if the node should
    // exist after all updates have been applied.
    return (data->DoesNodeExpectNodeWillBeDestroyed() || IsRemovedNode(node)) &&
           data->node_exists;
  }

  // Returns true if the node should exist in the tree but doesn't have
  // any node data yet.
  bool DoesPendingNodeRequireInit(AXNode::AXID node_id) const {
    DCHECK_EQ(AXTreePendingStructureStatus::kComputing, pending_update_status)
        << "This method should only be called while computing pending changes, "
           "before updates are made to the tree.";
    PendingStructureChanges* data = GetPendingStructureChanges(node_id);
    return data && data->DoesNodeRequireInit();
  }

  // Returns the parent node id for the pending node.
  base::Optional<AXNode::AXID> GetParentIdForPendingNode(AXNode::AXID node_id) {
    DCHECK_EQ(AXTreePendingStructureStatus::kComputing, pending_update_status)
        << "This method should only be called while computing pending changes, "
           "before updates are made to the tree.";
    PendingStructureChanges* data = GetOrCreatePendingStructureChanges(node_id);
    DCHECK(!data->parent_node_id ||
           ShouldPendingNodeExistInTree(*data->parent_node_id));
    return data->parent_node_id;
  }

  // Returns true if this node should exist in the tree.
  bool ShouldPendingNodeExistInTree(AXNode::AXID node_id) {
    DCHECK_EQ(AXTreePendingStructureStatus::kComputing, pending_update_status)
        << "This method should only be called while computing pending changes, "
           "before updates are made to the tree.";
    return GetOrCreatePendingStructureChanges(node_id)->node_exists;
  }

  // Returns the last known node data for a pending node.
  const AXNodeData& GetLastKnownPendingNodeData(AXNode::AXID node_id) const {
    DCHECK_EQ(AXTreePendingStructureStatus::kComputing, pending_update_status)
        << "This method should only be called while computing pending changes, "
           "before updates are made to the tree.";
    static base::NoDestructor<ui::AXNodeData> empty_data;
    PendingStructureChanges* data = GetPendingStructureChanges(node_id);
    return (data && data->last_known_data) ? *data->last_known_data
                                           : *empty_data;
  }

  // Clear the last known pending data for |node_id|.
  void ClearLastKnownPendingNodeData(AXNode::AXID node_id) {
    DCHECK_EQ(AXTreePendingStructureStatus::kComputing, pending_update_status)
        << "This method should only be called while computing pending changes, "
           "before updates are made to the tree.";
    GetOrCreatePendingStructureChanges(node_id)->last_known_data = nullptr;
  }

  // Update the last known pending node data for |node_data.id|.
  void SetLastKnownPendingNodeData(const AXNodeData* node_data) {
    DCHECK_EQ(AXTreePendingStructureStatus::kComputing, pending_update_status)
        << "This method should only be called while computing pending changes, "
           "before updates are made to the tree.";
    GetOrCreatePendingStructureChanges(node_data->id)->last_known_data =
        node_data;
  }

  // Returns the number of times the update is expected to destroy a
  // subtree rooted at |node_id|.
  int32_t GetPendingDestroySubtreeCount(AXNode::AXID node_id) const {
    DCHECK_EQ(AXTreePendingStructureStatus::kComplete, pending_update_status)
        << "This method should not be called before pending changes have "
           "finished computing.";
    if (PendingStructureChanges* data = GetPendingStructureChanges(node_id))
      return data->destroy_subtree_count;
    return 0;
  }

  // Increments the number of times the update is expected to
  // destroy a subtree rooted at |node_id|.
  // Returns true on success, false on failure when the node will not exist.
  bool IncrementPendingDestroySubtreeCount(AXNode::AXID node_id) {
    DCHECK_EQ(AXTreePendingStructureStatus::kComputing, pending_update_status)
        << "This method should only be called while computing pending changes, "
           "before updates are made to the tree.";
    PendingStructureChanges* data = GetOrCreatePendingStructureChanges(node_id);
    if (!data->node_exists)
      return false;

    ++data->destroy_subtree_count;
    return true;
  }

  // Decrements the number of times the update is expected to
  // destroy a subtree rooted at |node_id|.
  void DecrementPendingDestroySubtreeCount(AXNode::AXID node_id) {
    DCHECK_EQ(AXTreePendingStructureStatus::kComplete, pending_update_status)
        << "This method should not be called before pending changes have "
           "finished computing.";
    if (PendingStructureChanges* data = GetPendingStructureChanges(node_id)) {
      DCHECK_GT(data->destroy_subtree_count, 0);
      --data->destroy_subtree_count;
    }
  }

  // Returns the number of times the update is expected to destroy
  // a node with |node_id|.
  int32_t GetPendingDestroyNodeCount(AXNode::AXID node_id) const {
    DCHECK_EQ(AXTreePendingStructureStatus::kComplete, pending_update_status)
        << "This method should not be called before pending changes have "
           "finished computing.";
    if (PendingStructureChanges* data = GetPendingStructureChanges(node_id))
      return data->destroy_node_count;
    return 0;
  }

  // Increments the number of times the update is expected to
  // destroy a node with |node_id|.
  // Returns true on success, false on failure when the node will not exist.
  bool IncrementPendingDestroyNodeCount(AXNode::AXID node_id) {
    DCHECK_EQ(AXTreePendingStructureStatus::kComputing, pending_update_status)
        << "This method should only be called while computing pending changes, "
           "before updates are made to the tree.";
    PendingStructureChanges* data = GetOrCreatePendingStructureChanges(node_id);
    if (!data->node_exists)
      return false;

    ++data->destroy_node_count;
    data->node_exists = false;
    data->last_known_data = nullptr;
    data->parent_node_id = base::nullopt;
    if (pending_root_id == node_id)
      pending_root_id = base::nullopt;
    return true;
  }

  // Decrements the number of times the update is expected to
  // destroy a node with |node_id|.
  void DecrementPendingDestroyNodeCount(AXNode::AXID node_id) {
    DCHECK_EQ(AXTreePendingStructureStatus::kComplete, pending_update_status)
        << "This method should not be called before pending changes have "
           "finished computing.";
    if (PendingStructureChanges* data = GetPendingStructureChanges(node_id)) {
      DCHECK_GT(data->destroy_node_count, 0);
      --data->destroy_node_count;
    }
  }

  // Returns the number of times the update is expected to create
  // a node with |node_id|.
  int32_t GetPendingCreateNodeCount(AXNode::AXID node_id) const {
    DCHECK_EQ(AXTreePendingStructureStatus::kComplete, pending_update_status)
        << "This method should not be called before pending changes have "
           "finished computing.";
    if (PendingStructureChanges* data = GetPendingStructureChanges(node_id))
      return data->create_node_count;
    return 0;
  }

  // Increments the number of times the update is expected to
  // create a node with |node_id|.
  // Returns true on success, false on failure when the node will already exist.
  bool IncrementPendingCreateNodeCount(
      AXNode::AXID node_id,
      base::Optional<AXNode::AXID> parent_node_id) {
    DCHECK_EQ(AXTreePendingStructureStatus::kComputing, pending_update_status)
        << "This method should only be called while computing pending changes, "
           "before updates are made to the tree.";
    PendingStructureChanges* data = GetOrCreatePendingStructureChanges(node_id);
    if (data->node_exists)
      return false;

    ++data->create_node_count;
    data->node_exists = true;
    data->parent_node_id = parent_node_id;
    return true;
  }

  // Decrements the number of times the update is expected to
  // create a node with |node_id|.
  void DecrementPendingCreateNodeCount(AXNode::AXID node_id) {
    DCHECK_EQ(AXTreePendingStructureStatus::kComplete, pending_update_status)
        << "This method should not be called before pending changes have "
           "finished computing.";
    if (PendingStructureChanges* data = GetPendingStructureChanges(node_id)) {
      DCHECK_GT(data->create_node_count, 0);
      --data->create_node_count;
    }
  }

  // Returns whether this update must invalidate the unignored cached
  // values for |node_id|.
  bool InvalidatesUnignoredCachedValues(AXNode::AXID node_id) {
    return base::Contains(invalidate_unignored_cached_values_ids, node_id);
  }

  // Adds the parent of |node_id| to the list of nodes to invalidate unignored
  // cached values.
  void InvalidateParentNodeUnignoredCacheValues(AXNode::AXID node_id) {
    DCHECK_EQ(AXTreePendingStructureStatus::kComputing, pending_update_status)
        << "This method should only be called while computing pending changes, "
           "before updates are made to the tree.";
    base::Optional<AXNode::AXID> parent_node_id =
        GetParentIdForPendingNode(node_id);
    if (parent_node_id) {
      invalidate_unignored_cached_values_ids.insert(*parent_node_id);
    }
  }

  // Indicates the status for calculating what changes will occur during
  // an update before the update applies changes.
  AXTreePendingStructureStatus pending_update_status;

  // Keeps track of the root node id when calculating what changes will occur
  // during an update before the update applies changes.
  base::Optional<AXNode::AXID> pending_root_id;

  // Keeps track of whether the root node will need to be created as a new node.
  // This may occur either when the root node does not exist before applying
  // updates to the tree (new tree), or if the root is the |node_id_to_clear|
  // and will be destroyed before applying AXNodeData updates to the tree.
  bool root_will_be_created;

  // During an update, this keeps track of all nodes that have been
  // implicitly referenced as part of this update, but haven't been
  // updated yet. It's an error if there are any pending nodes at the
  // end of Unserialize.
  std::set<AXNode::AXID> pending_nodes;

  // Keeps track of nodes whose cached unignored child count, or unignored
  // index in parent may have changed, and must be updated.
  std::set<AXNode::AXID> invalidate_unignored_cached_values_ids;

  // Keeps track of nodes that have changed their node data.
  std::set<AXNode::AXID> node_data_changed_ids;

  // Keeps track of new nodes created during this update.
  std::set<AXNode::AXID> new_node_ids;

  // Keeps track of any nodes removed. Nodes are removed when their AXID no
  // longer exist in the parent |child_ids| list, or the node is part of to the
  // subtree of the AXID that was explicitally cleared with |node_id_to_clear|.
  // Used to identify re-parented nodes. A re-parented occurs when any AXID
  // is first removed from the tree then added to the tree again.
  std::set<AXNode::AXID> removed_node_ids;

  // Maps between a node id and its pending update information.
  std::map<AXNode::AXID, std::unique_ptr<PendingStructureChanges>>
      node_id_to_pending_data;

  // Maps between a node id and the data it owned before being updated.
  // We need to keep this around in order to correctly fire post-update events.
  std::map<AXNode::AXID, AXNodeData> old_node_id_to_data;

  // Optional copy of the old tree data, only populated when the tree
  // data has changed.
  base::Optional<AXTreeData> old_tree_data;

 private:
  PendingStructureChanges* GetPendingStructureChanges(
      AXNode::AXID node_id) const {
    auto iter = node_id_to_pending_data.find(node_id);
    return (iter != node_id_to_pending_data.cend()) ? iter->second.get()
                                                    : nullptr;
  }

  PendingStructureChanges* GetOrCreatePendingStructureChanges(
      AXNode::AXID node_id) {
    auto iter = node_id_to_pending_data.find(node_id);
    if (iter == node_id_to_pending_data.cend()) {
      const AXNode* node = tree.GetFromId(node_id);
      iter = node_id_to_pending_data
                 .emplace(std::make_pair(
                     node_id, std::make_unique<PendingStructureChanges>(node)))
                 .first;
    }
    return iter->second.get();
  }

  // We need to hold onto a reference to the AXTree so that we can
  // lazily initialize |PendingStructureChanges| objects.
  const AXTree& tree;
};

AXTree::AXTree() {
  AXNodeData root;
  root.id = AXNode::kInvalidAXID;

  AXTreeUpdate initial_state;
  initial_state.root_id = AXNode::kInvalidAXID;
  initial_state.nodes.push_back(root);
  CHECK(Unserialize(initial_state)) << error();
  // TODO(chrishall): should language_detection_manager be a member or pointer?
  // TODO(chrishall): do we want to initialize all the time, on demand, or only
  //                  when feature flag is set?
  DCHECK(!language_detection_manager);
  language_detection_manager =
      std::make_unique<AXLanguageDetectionManager>(this);
}

AXTree::AXTree(const AXTreeUpdate& initial_state) {
  CHECK(Unserialize(initial_state)) << error();
  DCHECK(!language_detection_manager);
  language_detection_manager =
      std::make_unique<AXLanguageDetectionManager>(this);
}

AXTree::~AXTree() {
  if (root_) {
    RecursivelyNotifyNodeDeletedForTreeTeardown(root_);
    base::AutoReset<bool> update_state_resetter(&tree_update_in_progress_,
                                                true);
    DestroyNodeAndSubtree(root_, nullptr);
  }
  for (auto& entry : table_info_map_)
    delete entry.second;
  table_info_map_.clear();
}

void AXTree::AddObserver(AXTreeObserver* observer) {
  observers_.AddObserver(observer);
}

bool AXTree::HasObserver(AXTreeObserver* observer) {
  return observers_.HasObserver(observer);
}

void AXTree::RemoveObserver(const AXTreeObserver* observer) {
  observers_.RemoveObserver(observer);
}

AXTreeID AXTree::GetAXTreeID() const {
  return data().tree_id;
}

AXNode* AXTree::GetFromId(int32_t id) const {
  auto iter = id_map_.find(id);
  return iter != id_map_.end() ? iter->second : nullptr;
}

void AXTree::UpdateData(const AXTreeData& new_data) {
  if (data_ == new_data)
    return;

  AXTreeData old_data = data_;
  data_ = new_data;
  for (AXTreeObserver& observer : observers_)
    observer.OnTreeDataChanged(this, old_data, new_data);
}

gfx::RectF AXTree::RelativeToTreeBoundsInternal(const AXNode* node,
                                                gfx::RectF bounds,
                                                bool* offscreen,
                                                bool clip_bounds,
                                                bool allow_recursion) const {
  // If |bounds| is uninitialized, which is not the same as empty,
  // start with the node bounds.
  if (bounds.width() == 0 && bounds.height() == 0) {
    bounds = node->data().relative_bounds.bounds;

    // If the node bounds is empty (either width or height is zero),
    // try to compute good bounds from the children.
    // If a tree update is in progress, skip this step as children may be in a
    // bad state.
    if (bounds.IsEmpty() && !GetTreeUpdateInProgressState() &&
        allow_recursion) {
      for (size_t i = 0; i < node->children().size(); i++) {
        ui::AXNode* child = node->children()[i];

        bool ignore_offscreen;
        gfx::RectF child_bounds = RelativeToTreeBoundsInternal(
            child, gfx::RectF(), &ignore_offscreen, clip_bounds,
            /* allow_recursion = */ false);
        bounds.Union(child_bounds);
      }
      if (bounds.width() > 0 && bounds.height() > 0) {
        return bounds;
      }
    }
  } else {
    bounds.Offset(node->data().relative_bounds.bounds.x(),
                  node->data().relative_bounds.bounds.y());
  }

  const AXNode* original_node = node;
  while (node != nullptr) {
    if (node->data().relative_bounds.transform)
      node->data().relative_bounds.transform->TransformRect(&bounds);
    // Apply any transforms and offsets for each node and then walk up to
    // its offset container. If no offset container is specified, coordinates
    // are relative to the root node.
    const AXNode* container =
        GetFromId(node->data().relative_bounds.offset_container_id);
    if (!container && container != root())
      container = root();
    if (!container || container == node)
      break;

    gfx::RectF container_bounds = container->data().relative_bounds.bounds;
    bounds.Offset(container_bounds.x(), container_bounds.y());

    int scroll_x = 0;
    int scroll_y = 0;
    if (container->data().GetIntAttribute(ax::mojom::IntAttribute::kScrollX,
                                          &scroll_x) &&
        container->data().GetIntAttribute(ax::mojom::IntAttribute::kScrollY,
                                          &scroll_y)) {
      bounds.Offset(-scroll_x, -scroll_y);
    }

    // Get the intersection between the bounds and the container.
    gfx::RectF intersection = bounds;
    intersection.Intersect(container_bounds);

    // Calculate the clipped bounds to determine offscreen state.
    gfx::RectF clipped = bounds;
    // If this node has the kClipsChildren attribute set, clip the rect to fit.
    if (container->data().GetBoolAttribute(
            ax::mojom::BoolAttribute::kClipsChildren)) {
      if (!intersection.IsEmpty()) {
        // We can simply clip it to the container.
        clipped = intersection;
      } else {
        // Totally offscreen. Find the nearest edge or corner.
        // Make the minimum dimension 1 instead of 0.
        if (clipped.x() >= container_bounds.width()) {
          clipped.set_x(container_bounds.right() - 1);
          clipped.set_width(1);
        } else if (clipped.x() + clipped.width() <= 0) {
          clipped.set_x(container_bounds.x());
          clipped.set_width(1);
        }
        if (clipped.y() >= container_bounds.height()) {
          clipped.set_y(container_bounds.bottom() - 1);
          clipped.set_height(1);
        } else if (clipped.y() + clipped.height() <= 0) {
          clipped.set_y(container_bounds.y());
          clipped.set_height(1);
        }
      }
    }

    if (clip_bounds)
      bounds = clipped;

    if (container->data().GetBoolAttribute(
            ax::mojom::BoolAttribute::kClipsChildren) &&
        intersection.IsEmpty() && !clipped.IsEmpty()) {
      // If it is offscreen with respect to its parent, and the node itself is
      // not empty, label it offscreen.
      // Here we are extending the definition of offscreen to include elements
      // that are clipped by their parents in addition to those clipped by
      // the rootWebArea.
      // No need to update |offscreen| if |intersection| is not empty, because
      // it should be false by default.
      if (offscreen != nullptr)
        *offscreen |= true;
    }

    node = container;
  }

  // If we don't have any size yet, try to adjust the bounds to fill the
  // nearest ancestor that does have bounds.
  //
  // The rationale is that it's not useful to the user for an object to
  // have no width or height and it's probably a bug; it's better to
  // reflect the bounds of the nearest ancestor rather than a 0x0 box.
  // Tag this node as 'offscreen' because it has no true size, just a
  // size inherited from the ancestor.
  if (bounds.width() == 0 && bounds.height() == 0) {
    const AXNode* ancestor = original_node->parent();
    gfx::RectF ancestor_bounds;
    while (ancestor) {
      ancestor_bounds = ancestor->data().relative_bounds.bounds;
      if (ancestor_bounds.width() > 0 || ancestor_bounds.height() > 0)
        break;
      ancestor = ancestor->parent();
    }

    if (ancestor && allow_recursion) {
      bool ignore_offscreen;
      bool allow_recursion = false;
      ancestor_bounds = RelativeToTreeBoundsInternal(
          ancestor, gfx::RectF(), &ignore_offscreen, clip_bounds,
          allow_recursion);

      gfx::RectF original_bounds = original_node->data().relative_bounds.bounds;
      if (original_bounds.x() == 0 && original_bounds.y() == 0) {
        bounds = ancestor_bounds;
      } else {
        bounds.set_width(std::max(0.0f, ancestor_bounds.right() - bounds.x()));
        bounds.set_height(
            std::max(0.0f, ancestor_bounds.bottom() - bounds.y()));
      }
      if (offscreen != nullptr)
        *offscreen |= true;
    }
  }

  return bounds;
}

gfx::RectF AXTree::RelativeToTreeBounds(const AXNode* node,
                                        gfx::RectF bounds,
                                        bool* offscreen,
                                        bool clip_bounds) const {
  bool allow_recursion = true;
  return RelativeToTreeBoundsInternal(node, bounds, offscreen, clip_bounds,
                                      allow_recursion);
}

gfx::RectF AXTree::GetTreeBounds(const AXNode* node,
                                 bool* offscreen,
                                 bool clip_bounds) const {
  return RelativeToTreeBounds(node, gfx::RectF(), offscreen, clip_bounds);
}

std::set<int32_t> AXTree::GetReverseRelations(ax::mojom::IntAttribute attr,
                                              int32_t dst_id) const {
  DCHECK(IsNodeIdIntAttribute(attr));

  // Conceptually, this is the "const" version of:
  //   return int_reverse_relations_[attr][dst_id];
  const auto& attr_relations = int_reverse_relations_.find(attr);
  if (attr_relations != int_reverse_relations_.end()) {
    const auto& result = attr_relations->second.find(dst_id);
    if (result != attr_relations->second.end())
      return result->second;
  }
  return std::set<int32_t>();
}

std::set<int32_t> AXTree::GetReverseRelations(ax::mojom::IntListAttribute attr,
                                              int32_t dst_id) const {
  DCHECK(IsNodeIdIntListAttribute(attr));

  // Conceptually, this is the "const" version of:
  //   return intlist_reverse_relations_[attr][dst_id];
  const auto& attr_relations = intlist_reverse_relations_.find(attr);
  if (attr_relations != intlist_reverse_relations_.end()) {
    const auto& result = attr_relations->second.find(dst_id);
    if (result != attr_relations->second.end())
      return result->second;
  }
  return std::set<int32_t>();
}

std::set<int32_t> AXTree::GetNodeIdsForChildTreeId(
    AXTreeID child_tree_id) const {
  // Conceptually, this is the "const" version of:
  //   return child_tree_id_reverse_map_[child_tree_id];
  const auto& result = child_tree_id_reverse_map_.find(child_tree_id);
  if (result != child_tree_id_reverse_map_.end())
    return result->second;
  return std::set<int32_t>();
}

const std::set<AXTreeID> AXTree::GetAllChildTreeIds() const {
  std::set<AXTreeID> result;
  for (auto entry : child_tree_id_reverse_map_)
    result.insert(entry.first);
  return result;
}

bool AXTree::Unserialize(const AXTreeUpdate& update) {
  AXTreeUpdateState update_state(*this);
  const AXNode::AXID old_root_id = root_ ? root_->id() : AXNode::kInvalidAXID;

  // Accumulates the work that will be required to update the AXTree.
  // This allows us to notify observers of structure changes when the
  // tree is still in a stable and unchanged state.
  if (!ComputePendingChanges(update, update_state))
    return false;

  // Notify observers of subtrees and nodes that are about to be destroyed or
  // reparented, this must be done before applying any updates to the tree.
  for (auto&& pair : update_state.node_id_to_pending_data) {
    const AXNode::AXID node_id = pair.first;
    const std::unique_ptr<PendingStructureChanges>& data = pair.second;
    if (data->DoesNodeExpectSubtreeOrNodeWillBeDestroyed()) {
      if (AXNode* node = GetFromId(node_id)) {
        if (data->DoesNodeExpectSubtreeWillBeDestroyed())
          NotifySubtreeWillBeReparentedOrDeleted(node, &update_state);
        if (data->DoesNodeExpectNodeWillBeDestroyed())
          NotifyNodeWillBeReparentedOrDeleted(node, &update_state);
      }
    }
  }

  // Notify observers of nodes that are about to change their data.
  // This must be done before applying any updates to the tree.
  // This is iterating in reverse order so that we only notify once per node id,
  // and that we only notify the initial node data against the final node data,
  // unless the node is a new root.
  std::set<int32_t> notified_node_data_will_change;
  for (size_t i = update.nodes.size(); i-- > 0;) {
    const AXNodeData& new_data = update.nodes[i];
    const bool is_new_root =
        update_state.root_will_be_created && new_data.id == update.root_id;
    if (!is_new_root) {
      AXNode* node = GetFromId(new_data.id);
      if (node && notified_node_data_will_change.insert(new_data.id).second)
        NotifyNodeDataWillChange(node->data(), new_data);
    }
  }

  // Now that we have finished sending events for changes that will  happen,
  // set update state to true. |tree_update_in_progress_| gets set back to
  // false whenever this function exits.
  base::AutoReset<bool> update_state_resetter(&tree_update_in_progress_, true);

  // Handle |node_id_to_clear| before applying ordinary node updates.
  // We distinguish between updating the root, e.g. changing its children or
  // some of its attributes, or replacing the root completely. If the root is
  // being updated, update.node_id_to_clear should hold the current root's ID.
  // Otherwise if the root is being replaced, update.root_id should hold the ID
  // of the new root.
  bool root_updated = false;
  if (update.node_id_to_clear != AXNode::kInvalidAXID) {
    if (AXNode* cleared_node = GetFromId(update.node_id_to_clear)) {
      DCHECK(root_);
      if (cleared_node == root_) {
        // Only destroy the root if the root was replaced and not if it's simply
        // updated. To figure out if the root was simply updated, we compare
        // the ID of the new root with the existing root ID.
        if (update.root_id != old_root_id) {
          // Clear root_ before calling DestroySubtree so that root_ doesn't
          // ever point to an invalid node.
          AXNode* old_root = root_;
          root_ = nullptr;
          DestroySubtree(old_root, &update_state);
        } else {
          // If the root has simply been updated, we treat it like an update to
          // any other node.
          root_updated = true;
        }
      }

      // If the tree doesn't exists any more because the root has just been
      // replaced, there is nothing more to clear.
      if (root_) {
        for (auto* child : cleared_node->children())
          DestroySubtree(child, &update_state);
        std::vector<AXNode*> children;
        cleared_node->SwapChildren(children);
        update_state.pending_nodes.insert(cleared_node->id());
      }
    }
  }

  DCHECK_EQ(!GetFromId(update.root_id), update_state.root_will_be_created);

  // Update the tree data, do not call |UpdateData| since we want to defer
  // the |OnTreeDataChanged| event until after the tree has finished updating.
  if (update.has_tree_data && data_ != update.tree_data) {
    update_state.old_tree_data = data_;
    data_ = update.tree_data;
  }

  // Update all of the nodes in the update.
  for (size_t i = 0; i < update.nodes.size(); ++i) {
    const bool is_new_root = update_state.root_will_be_created &&
                             update.nodes[i].id == update.root_id;
    if (!UpdateNode(update.nodes[i], is_new_root, &update_state))
      return false;
  }

  if (!root_) {
    error_ = "Tree has no root.";
    return false;
  }

  if (!ValidatePendingChangesComplete(update_state))
    return false;

  // Look for changes to nodes that are a descendant of a table,
  // and invalidate their table info if so.  We have to walk up the
  // ancestry of every node that was updated potentially, so keep track of
  // ids that were checked to eliminate duplicate work.
  std::set<int32_t> table_ids_checked;
  for (size_t i = 0; i < update.nodes.size(); ++i) {
    AXNode* node = GetFromId(update.nodes[i].id);
    while (node) {
      if (table_ids_checked.find(node->id()) != table_ids_checked.end())
        break;
      // Remove any table infos.
      const auto& table_info_entry = table_info_map_.find(node->id());
      if (table_info_entry != table_info_map_.end())
        table_info_entry->second->Invalidate();
      table_ids_checked.insert(node->id());
      node = node->parent();
    }
  }

  // Clear list_info_map_
  ordered_set_info_map_.clear();

  std::vector<AXTreeObserver::Change> changes;
  changes.reserve(update.nodes.size());
  std::set<AXNode::AXID> visited_observer_changes;
  for (size_t i = 0; i < update.nodes.size(); ++i) {
    AXNode* node = GetFromId(update.nodes[i].id);
    if (!node || !visited_observer_changes.emplace(update.nodes[i].id).second)
      continue;

    bool is_new_node = update_state.IsCreatedNode(node);
    bool is_reparented_node = update_state.IsReparentedNode(node);

    AXTreeObserver::ChangeType change = AXTreeObserver::NODE_CHANGED;
    if (is_new_node) {
      if (is_reparented_node) {
        // A reparented subtree is any new node whose parent either doesn't
        // exist, or whose parent is not new.
        // Note that we also need to check for the special case when we update
        // the root without replacing it.
        bool is_subtree = !node->parent() ||
                          !update_state.IsCreatedNode(node->parent()) ||
                          (node->parent() == root_ && root_updated);
        change = is_subtree ? AXTreeObserver::SUBTREE_REPARENTED
                            : AXTreeObserver::NODE_REPARENTED;
      } else {
        // A new subtree is any new node whose parent is either not new, or
        // whose parent happens to be new only because it has been reparented.
        // Note that we also need to check for the special case when we update
        // the root without replacing it.
        bool is_subtree = !node->parent() ||
                          !update_state.IsCreatedNode(node->parent()) ||
                          update_state.IsRemovedNode(node->parent()) ||
                          (node->parent() == root_ && root_updated);
        change = is_subtree ? AXTreeObserver::SUBTREE_CREATED
                            : AXTreeObserver::NODE_CREATED;
      }
    }
    changes.push_back(AXTreeObserver::Change(node, change));
  }

  // Update the unignored cached values as necessary, ensuring that we only
  // update once for each unignored node.
  // If the node is ignored, we must update from an unignored ancestor.
  std::set<AXNode::AXID> updated_unignored_cached_values_ids;
  for (AXNode::AXID node_id :
       update_state.invalidate_unignored_cached_values_ids) {
    AXNode* node = GetFromId(node_id);
    while (node && node->data().HasState(ax::mojom::State::kIgnored))
      node = node->parent();
    if (node && updated_unignored_cached_values_ids.insert(node->id()).second)
      node->UpdateUnignoredCachedValues();
  }

  // Tree is no longer updating.
  SetTreeUpdateInProgressState(false);

  // Now that the tree is stable and its nodes have been updated, notify if
  // the tree data changed. We must do this after updating nodes in case the
  // root has been replaced, so observers have the most up-to-date information.
  if (update_state.old_tree_data) {
    for (AXTreeObserver& observer : observers_)
      observer.OnTreeDataChanged(this, *update_state.old_tree_data, data_);
  }

  // Now that the unignored cached values are up to date, update observers to
  // the nodes that were deleted from the tree but not reparented.
  for (AXNode::AXID node_id : update_state.removed_node_ids) {
    if (!update_state.IsCreatedNode(node_id))
      NotifyNodeHasBeenDeleted(node_id);
  }

  // Now that the unignored cached values are up to date, update observers to
  // new nodes in the tree.
  for (AXNode::AXID node_id : update_state.new_node_ids)
    NotifyNodeHasBeenReparentedOrCreated(GetFromId(node_id), &update_state);

  // Now that the unignored cached values are up to date, update observers to
  // node changes.
  for (AXNode::AXID node_data_changed_id : update_state.node_data_changed_ids) {
    AXNode* node = GetFromId(node_data_changed_id);
    DCHECK(node);

    // If the node exists and is in the old data map, then the node data
    // may have changed unless this is a new root.
    const bool is_new_root = update_state.root_will_be_created &&
                             node_data_changed_id == update.root_id;
    if (!is_new_root) {
      auto it = update_state.old_node_id_to_data.find(node_data_changed_id);
      if (it != update_state.old_node_id_to_data.end()) {
        const AXNodeData& old_node_data = it->second;
        NotifyNodeDataHasBeenChanged(node, old_node_data, node->data());
      }
    }

    // |OnNodeChanged| should be fired for all nodes that have been updated.
    for (AXTreeObserver& observer : observers_)
      observer.OnNodeChanged(this, node);
  }

  for (AXTreeObserver& observer : observers_)
    observer.OnAtomicUpdateFinished(this, root_->id() != old_root_id, changes);

  return true;
}

AXTableInfo* AXTree::GetTableInfo(const AXNode* const_table_node) const {
  DCHECK(!GetTreeUpdateInProgressState());
  // Note: the const_casts are here because we want this function to be able
  // to be called from a const virtual function on AXNode. AXTableInfo is
  // computed on demand and cached, but that's an implementation detail
  // we want to hide from users of this API.
  AXNode* table_node = const_cast<AXNode*>(const_table_node);
  AXTree* tree = const_cast<AXTree*>(this);

  DCHECK(table_node);
  const auto& cached = table_info_map_.find(table_node->id());
  if (cached != table_info_map_.end()) {
    // Get existing table info, and update if invalid because the
    // tree has changed since the last time we accessed it.
    AXTableInfo* table_info = cached->second;
    if (!table_info->valid()) {
      bool success = table_info->Update();
      if (!success) {
        // If Update() returned false, this is no longer a valid table.
        // Remove it from the map.
        delete table_info;
        table_info = nullptr;
        table_info_map_.erase(table_node->id());
      }
      // See note about const_cast, above.
      for (AXTreeObserver& observer : observers_)
        observer.OnNodeChanged(tree, table_node);
    }
    return table_info;
  }

  AXTableInfo* table_info = AXTableInfo::Create(tree, table_node);
  if (!table_info)
    return nullptr;

  table_info_map_[table_node->id()] = table_info;
  for (AXTreeObserver& observer : observers_)
    observer.OnNodeChanged(tree, table_node);

  return table_info;
}

std::string AXTree::ToString() const {
  return "AXTree" + data_.ToString() + "\n" + TreeToStringHelper(root_, 0);
}

AXNode* AXTree::CreateNode(AXNode* parent,
                           AXNode::AXID id,
                           size_t index_in_parent,
                           AXTreeUpdateState* update_state) {
  DCHECK(GetTreeUpdateInProgressState());
  // |update_state| must already contain information about all of the expected
  // changes and invalidations to apply. If any of these are missing, observers
  // may not be notified of changes.
  DCHECK(!GetFromId(id));
  DCHECK_GT(update_state->GetPendingCreateNodeCount(id), 0);
  DCHECK(update_state->InvalidatesUnignoredCachedValues(id));
  DCHECK(!parent ||
         update_state->InvalidatesUnignoredCachedValues(parent->id()));
  update_state->DecrementPendingCreateNodeCount(id);
  update_state->new_node_ids.insert(id);
  // If this node is the root, use the given index_in_parent as the unignored
  // index in parent to provide consistency with index_in_parent.
  AXNode* new_node = new AXNode(this, parent, id, index_in_parent,
                                parent ? 0 : index_in_parent);
  id_map_[new_node->id()] = new_node;
  return new_node;
}

bool AXTree::ComputePendingChanges(const AXTreeUpdate& update,
                                   AXTreeUpdateState& update_state) {
  DCHECK_EQ(AXTreePendingStructureStatus::kNotStarted,
            update_state.pending_update_status)
      << "Pending changes have already started being computed.";
  update_state.pending_update_status = AXTreePendingStructureStatus::kComputing;

  base::AutoReset<base::Optional<AXNode::AXID>> pending_root_id_resetter(
      &update_state.pending_root_id,
      root_ ? base::Optional<AXNode::AXID>{root_->id()} : base::nullopt);

  // We distinguish between updating the root, e.g. changing its children or
  // some of its attributes, or replacing the root completely. If the root is
  // being updated, update.node_id_to_clear should hold the current root's ID.
  // Otherwise if the root is being replaced, update.root_id should hold the ID
  // of the new root.
  if (update.node_id_to_clear != AXNode::kInvalidAXID) {
    if (AXNode* cleared_node = GetFromId(update.node_id_to_clear)) {
      DCHECK(root_);
      if (cleared_node == root_ &&
          update.root_id != update_state.pending_root_id) {
        // Only destroy the root if the root was replaced and not if it's simply
        // updated. To figure out if the root was simply updated, we compare
        // the ID of the new root with the existing root ID.
        MarkSubtreeForDestruction(*update_state.pending_root_id, &update_state);
      }

      // If the tree has been marked for destruction because the root will be
      // replaced, there is nothing more to clear.
      if (update_state.ShouldPendingNodeExistInTree(root_->id())) {
        update_state.invalidate_unignored_cached_values_ids.insert(
            cleared_node->id());
        update_state.ClearLastKnownPendingNodeData(cleared_node->id());
        for (AXNode* child : cleared_node->children()) {
          MarkSubtreeForDestruction(child->id(), &update_state);
        }
      }
    }
  }

  update_state.root_will_be_created =
      !GetFromId(update.root_id) ||
      !update_state.ShouldPendingNodeExistInTree(update.root_id);

  // Populate |update_state| with all of the changes that will be performed
  // on the tree during the update.
  for (const AXNodeData& new_data : update.nodes) {
    bool is_new_root =
        update_state.root_will_be_created && new_data.id == update.root_id;
    if (!ComputePendingChangesToNode(new_data, is_new_root, &update_state)) {
      update_state.pending_update_status =
          AXTreePendingStructureStatus::kFailed;
      return false;
    }
  }

  update_state.pending_update_status = AXTreePendingStructureStatus::kComplete;
  return true;
}

bool AXTree::ComputePendingChangesToNode(const AXNodeData& new_data,
                                         bool is_new_root,
                                         AXTreeUpdateState* update_state) {
  // Compare every child's index in parent in the update with the existing
  // index in parent.  If the order has changed, invalidate the cached
  // unignored index in parent.
  for (size_t j = 0; j < new_data.child_ids.size(); j++) {
    AXNode* node = GetFromId(new_data.child_ids[j]);
    if (node && node->GetIndexInParent() != j)
      update_state->InvalidateParentNodeUnignoredCacheValues(node->id());
  }

  // If the node does not exist in the tree throw an error unless this
  // is the new root and it can be created.
  if (!update_state->ShouldPendingNodeExistInTree(new_data.id)) {
    if (!is_new_root) {
      error_ = base::StringPrintf(
          "%d will not be in the tree and is not the new root", new_data.id);
      return false;
    }

    // Creation is implicit for new root nodes. If |new_data.id| is already
    // pending for creation, then it must be a duplicate entry in the tree.
    if (!update_state->IncrementPendingCreateNodeCount(new_data.id,
                                                       base::nullopt)) {
      error_ = base::StringPrintf(
          "Node %d is already pending for creation, cannot be the new root",
          new_data.id);
      return false;
    }
    if (update_state->pending_root_id) {
      MarkSubtreeForDestruction(*update_state->pending_root_id, update_state);
    }
    update_state->pending_root_id = new_data.id;
  }

  // Create a set of new child ids so we can use it to find the nodes that
  // have been added and removed. Returns false if a duplicate is found.
  std::set<AXNode::AXID> new_child_id_set;
  for (AXNode::AXID new_child_id : new_data.child_ids) {
    if (base::Contains(new_child_id_set, new_child_id)) {
      error_ = base::StringPrintf("Node %d has duplicate child id %d",
                                  new_data.id, new_child_id);
      return false;
    }
    new_child_id_set.insert(new_child_id);
  }

  // If the node has not been initialized yet then its node data has either been
  // cleared when handling |node_id_to_clear|, or it's a new node.
  // In either case, all children must be created.
  if (update_state->DoesPendingNodeRequireInit(new_data.id)) {
    update_state->invalidate_unignored_cached_values_ids.insert(new_data.id);

    // If this node has been cleared via |node_id_to_clear| or is a new node,
    // the last-known parent's unignored cache needs to be updated.
    update_state->InvalidateParentNodeUnignoredCacheValues(new_data.id);

    for (AXNode::AXID child_id : new_child_id_set) {
      // If a |child_id| is already pending for creation, then it must be a
      // duplicate entry in the tree.
      update_state->invalidate_unignored_cached_values_ids.insert(child_id);
      if (!update_state->IncrementPendingCreateNodeCount(child_id,
                                                         new_data.id)) {
        error_ = base::StringPrintf(
            "Node %d is already pending for creation, cannot be a new child",
            child_id);
        return false;
      }
    }

    update_state->SetLastKnownPendingNodeData(&new_data);
    return true;
  }

  const AXNodeData& old_data =
      update_state->GetLastKnownPendingNodeData(new_data.id);

  // Create a set of old child ids so we can use it to find the nodes that
  // have been added and removed.
  std::set<AXNode::AXID> old_child_id_set(old_data.child_ids.cbegin(),
                                          old_data.child_ids.cend());

  std::vector<AXNode::AXID> create_or_destroy_ids;
  std::set_symmetric_difference(
      old_child_id_set.cbegin(), old_child_id_set.cend(),
      new_child_id_set.cbegin(), new_child_id_set.cend(),
      std::back_inserter(create_or_destroy_ids));

  // If the node has changed ignored state or there are any differences in
  // its children, then its unignored cached values must be invalidated.
  const bool ignored_changed = old_data.HasState(ax::mojom::State::kIgnored) !=
                               new_data.HasState(ax::mojom::State::kIgnored);
  if (!create_or_destroy_ids.empty() || ignored_changed) {
    update_state->invalidate_unignored_cached_values_ids.insert(new_data.id);

    // If this ignored state had changed also invalidate the parent.
    update_state->InvalidateParentNodeUnignoredCacheValues(new_data.id);
  }

  for (AXNode::AXID child_id : create_or_destroy_ids) {
    if (base::Contains(new_child_id_set, child_id)) {
      // This is a serious error - nodes should never be reparented without
      // first being removed from the tree. If a node exists in the tree already
      // then adding it to a new parent would mean stealing the node from its
      // old parent which hadn't been updated to reflect the change.
      if (update_state->ShouldPendingNodeExistInTree(child_id)) {
        error_ = base::StringPrintf(
            "Node %d is not marked for destruction, would be reparented to %d",
            child_id, new_data.id);
        return false;
      }

      // If a |child_id| is already pending for creation, then it must be a
      // duplicate entry in the tree.
      update_state->invalidate_unignored_cached_values_ids.insert(child_id);
      if (!update_state->IncrementPendingCreateNodeCount(child_id,
                                                         new_data.id)) {
        error_ = base::StringPrintf(
            "Node %d is already pending for creation, cannot be a new child",
            child_id);
        return false;
      }
    } else {
      // If |child_id| does not exist in the new set, then it has
      // been removed from |node|, and the subtree must be deleted.
      MarkSubtreeForDestruction(child_id, update_state);
    }
  }

  update_state->SetLastKnownPendingNodeData(&new_data);
  return true;
}

bool AXTree::UpdateNode(const AXNodeData& src,
                        bool is_new_root,
                        AXTreeUpdateState* update_state) {
  DCHECK(GetTreeUpdateInProgressState());
  // This method updates one node in the tree based on serialized data
  // received in an AXTreeUpdate. See AXTreeUpdate for pre and post
  // conditions.

  // Look up the node by id. If it's not found, then either the root
  // of the tree is being swapped, or we're out of sync with the source
  // and this is a serious error.
  AXNode* node = GetFromId(src.id);
  if (node) {
    update_state->pending_nodes.erase(node->id());
    UpdateReverseRelations(node, src);
    if (!update_state->IsCreatedNode(node) ||
        update_state->IsReparentedNode(node)) {
      update_state->old_node_id_to_data.insert(
          std::make_pair(node->id(), node->TakeData()));
    }
    node->SetData(src);
  } else {
    if (!is_new_root) {
      error_ = base::StringPrintf(
          "%d is not in the tree and not the new root", src.id);
      return false;
    }

    node = CreateNode(nullptr, src.id, 0, update_state);
    UpdateReverseRelations(node, src);
    node->SetData(src);
  }

  // If we come across a page breaking object, mark the tree as a paginated root
  if (src.GetBoolAttribute(ax::mojom::BoolAttribute::kIsPageBreakingObject))
    has_pagination_support_ = true;

  update_state->node_data_changed_ids.insert(node->id());

  // First, delete nodes that used to be children of this node but aren't
  // anymore.
  DeleteOldChildren(node, src.child_ids, update_state);

  // Now build a new children vector, reusing nodes when possible,
  // and swap it in.
  std::vector<AXNode*> new_children;
  bool success = CreateNewChildVector(
      node, src.child_ids, &new_children, update_state);
  node->SwapChildren(new_children);

  // Update the root of the tree if needed.
  if (is_new_root) {
    // Make sure root_ always points to something valid or null_, even inside
    // DestroySubtree.
    AXNode* old_root = root_;
    root_ = node;
    if (old_root && old_root != node)
      DestroySubtree(old_root, update_state);
  }

  return success;
}

void AXTree::NotifySubtreeWillBeReparentedOrDeleted(
    AXNode* node,
    const AXTreeUpdateState* update_state) {
  DCHECK(!GetTreeUpdateInProgressState());
  if (node->id() == AXNode::kInvalidAXID)
    return;

  for (AXTreeObserver& observer : observers_) {
    if (update_state->IsReparentedNode(node)) {
      observer.OnSubtreeWillBeReparented(this, node);
    } else {
      observer.OnSubtreeWillBeDeleted(this, node);
    }
  }
}

void AXTree::NotifyNodeWillBeReparentedOrDeleted(
    AXNode* node,
    const AXTreeUpdateState* update_state) {
  DCHECK(!GetTreeUpdateInProgressState());
  if (node->id() == AXNode::kInvalidAXID)
    return;

  for (AXTreeObserver& observer : observers_) {
    if (update_state->IsReparentedNode(node)) {
      observer.OnNodeWillBeReparented(this, node);
    } else {
      observer.OnNodeWillBeDeleted(this, node);
    }
  }
}

void AXTree::RecursivelyNotifyNodeDeletedForTreeTeardown(AXNode* node) {
  DCHECK(!GetTreeUpdateInProgressState());
  if (node->id() == AXNode::kInvalidAXID)
    return;

  for (AXTreeObserver& observer : observers_)
    observer.OnNodeDeleted(this, node->id());
  for (auto* child : node->children())
    RecursivelyNotifyNodeDeletedForTreeTeardown(child);
}

void AXTree::NotifyNodeHasBeenDeleted(AXNode::AXID node_id) {
  DCHECK(!GetTreeUpdateInProgressState());

  if (node_id == AXNode::kInvalidAXID)
    return;

  for (AXTreeObserver& observer : observers_)
    observer.OnNodeDeleted(this, node_id);
}

void AXTree::NotifyNodeHasBeenReparentedOrCreated(
    AXNode* node,
    const AXTreeUpdateState* update_state) {
  DCHECK(!GetTreeUpdateInProgressState());
  if (node->id() == AXNode::kInvalidAXID)
    return;

  for (AXTreeObserver& observer : observers_) {
    if (update_state->IsReparentedNode(node)) {
      observer.OnNodeReparented(this, node);
    } else {
      observer.OnNodeCreated(this, node);
    }
  }
}

void AXTree::NotifyNodeDataWillChange(const AXNodeData& old_data,
                                      const AXNodeData& new_data) {
  DCHECK(!GetTreeUpdateInProgressState());
  if (new_data.id == AXNode::kInvalidAXID)
    return;

  for (AXTreeObserver& observer : observers_)
    observer.OnNodeDataWillChange(this, old_data, new_data);
}

void AXTree::NotifyNodeDataHasBeenChanged(AXNode* node,
                                          const AXNodeData& old_data,
                                          const AXNodeData& new_data) {
  DCHECK(!GetTreeUpdateInProgressState());
  if (node->id() == AXNode::kInvalidAXID)
    return;

  for (AXTreeObserver& observer : observers_)
    observer.OnNodeDataChanged(this, old_data, new_data);

  if (old_data.role != new_data.role) {
    for (AXTreeObserver& observer : observers_)
      observer.OnRoleChanged(this, node, old_data.role, new_data.role);
  }

  if (old_data.state != new_data.state) {
    for (int32_t i = static_cast<int32_t>(ax::mojom::State::kNone) + 1;
         i <= static_cast<int32_t>(ax::mojom::State::kMaxValue); ++i) {
      ax::mojom::State state = static_cast<ax::mojom::State>(i);
      if (old_data.HasState(state) != new_data.HasState(state)) {
        for (AXTreeObserver& observer : observers_)
          observer.OnStateChanged(this, node, state, new_data.HasState(state));
      }
    }
  }

  auto string_callback = [this, node](ax::mojom::StringAttribute attr,
                                      const std::string& old_string,
                                      const std::string& new_string) {
    for (AXTreeObserver& observer : observers_) {
      observer.OnStringAttributeChanged(this, node, attr, old_string,
                                        new_string);
    }
  };
  CallIfAttributeValuesChanged(old_data.string_attributes,
                               new_data.string_attributes, std::string(),
                               string_callback);

  auto bool_callback = [this, node](ax::mojom::BoolAttribute attr,
                                    const bool& old_bool,
                                    const bool& new_bool) {
    for (AXTreeObserver& observer : observers_)
      observer.OnBoolAttributeChanged(this, node, attr, new_bool);
  };
  CallIfAttributeValuesChanged(old_data.bool_attributes,
                               new_data.bool_attributes, false, bool_callback);

  auto float_callback = [this, node](ax::mojom::FloatAttribute attr,
                                     const float& old_float,
                                     const float& new_float) {
    for (AXTreeObserver& observer : observers_)
      observer.OnFloatAttributeChanged(this, node, attr, old_float, new_float);
  };
  CallIfAttributeValuesChanged(old_data.float_attributes,
                               new_data.float_attributes, 0.0f, float_callback);

  auto int_callback = [this, node](ax::mojom::IntAttribute attr,
                                   const int& old_int, const int& new_int) {
    for (AXTreeObserver& observer : observers_)
      observer.OnIntAttributeChanged(this, node, attr, old_int, new_int);
  };
  CallIfAttributeValuesChanged(old_data.int_attributes, new_data.int_attributes,
                               0, int_callback);

  auto intlist_callback = [this, node](
                              ax::mojom::IntListAttribute attr,
                              const std::vector<int32_t>& old_intlist,
                              const std::vector<int32_t>& new_intlist) {
    for (AXTreeObserver& observer : observers_)
      observer.OnIntListAttributeChanged(this, node, attr, old_intlist,
                                         new_intlist);
  };
  CallIfAttributeValuesChanged(old_data.intlist_attributes,
                               new_data.intlist_attributes,
                               std::vector<int32_t>(), intlist_callback);

  auto stringlist_callback =
      [this, node](ax::mojom::StringListAttribute attr,
                   const std::vector<std::string>& old_stringlist,
                   const std::vector<std::string>& new_stringlist) {
        for (AXTreeObserver& observer : observers_)
          observer.OnStringListAttributeChanged(this, node, attr,
                                                old_stringlist, new_stringlist);
      };
  CallIfAttributeValuesChanged(old_data.stringlist_attributes,
                               new_data.stringlist_attributes,
                               std::vector<std::string>(), stringlist_callback);
}

void AXTree::UpdateReverseRelations(AXNode* node, const AXNodeData& new_data) {
  DCHECK(GetTreeUpdateInProgressState());
  const AXNodeData& old_data = node->data();
  int id = new_data.id;
  auto int_callback = [this, id](ax::mojom::IntAttribute attr,
                                 const int& old_id, const int& new_id) {
    if (!IsNodeIdIntAttribute(attr))
      return;

    // Remove old_id -> id from the map, and clear map keys if their
    // values are now empty.
    auto& map = int_reverse_relations_[attr];
    if (map.find(old_id) != map.end()) {
      map[old_id].erase(id);
      if (map[old_id].empty())
        map.erase(old_id);
    }

    // Add new_id -> id to the map, unless new_id is zero indicating that
    // we're only removing a relation.
    if (new_id)
      map[new_id].insert(id);
  };
  CallIfAttributeValuesChanged(old_data.int_attributes, new_data.int_attributes,
                               0, int_callback);

  auto intlist_callback = [this, id](ax::mojom::IntListAttribute attr,
                                     const std::vector<int32_t>& old_idlist,
                                     const std::vector<int32_t>& new_idlist) {
    if (!IsNodeIdIntListAttribute(attr))
      return;

    auto& map = intlist_reverse_relations_[attr];
    for (int32_t old_id : old_idlist) {
      if (map.find(old_id) != map.end()) {
        map[old_id].erase(id);
        if (map[old_id].empty())
          map.erase(old_id);
      }
    }
    for (int32_t new_id : new_idlist)
      intlist_reverse_relations_[attr][new_id].insert(id);
  };
  CallIfAttributeValuesChanged(old_data.intlist_attributes,
                               new_data.intlist_attributes,
                               std::vector<int32_t>(), intlist_callback);

  auto string_callback = [this, id](ax::mojom::StringAttribute attr,
                                    const std::string& old_string,
                                    const std::string& new_string) {
    if (attr == ax::mojom::StringAttribute::kChildTreeId) {
      // Remove old_string -> id from the map, and clear map keys if
      // their values are now empty.
      AXTreeID old_ax_tree_id = AXTreeID::FromString(old_string);
      if (child_tree_id_reverse_map_.find(old_ax_tree_id) !=
          child_tree_id_reverse_map_.end()) {
        child_tree_id_reverse_map_[old_ax_tree_id].erase(id);
        if (child_tree_id_reverse_map_[old_ax_tree_id].empty())
          child_tree_id_reverse_map_.erase(old_ax_tree_id);
      }

      // Add new_string -> id to the map, unless new_id is zero indicating that
      // we're only removing a relation.
      if (!new_string.empty()) {
        AXTreeID new_ax_tree_id = AXTreeID::FromString(new_string);
        child_tree_id_reverse_map_[new_ax_tree_id].insert(id);
      }
    }
  };

  CallIfAttributeValuesChanged(old_data.string_attributes,
                               new_data.string_attributes, std::string(),
                               string_callback);
}

bool AXTree::ValidatePendingChangesComplete(
    const AXTreeUpdateState& update_state) {
  if (!update_state.pending_nodes.empty()) {
    error_ = "Nodes left pending by the update:";
    for (const AXNode::AXID pending_id : update_state.pending_nodes)
      error_ += base::StringPrintf(" %d", pending_id);
    return false;
  }

  if (!update_state.node_id_to_pending_data.empty()) {
    std::string destroy_subtree_ids;
    std::string destroy_node_ids;
    std::string create_node_ids;

    bool has_pending_changes = false;
    for (auto&& pair : update_state.node_id_to_pending_data) {
      const AXNode::AXID pending_id = pair.first;
      const std::unique_ptr<PendingStructureChanges>& data = pair.second;
      if (data->DoesNodeExpectAnyStructureChanges()) {
        if (data->DoesNodeExpectSubtreeWillBeDestroyed())
          destroy_subtree_ids += base::StringPrintf(" %d", pending_id);
        if (data->DoesNodeExpectNodeWillBeDestroyed())
          destroy_node_ids += base::StringPrintf(" %d", pending_id);
        if (data->DoesNodeExpectNodeWillBeCreated())
          create_node_ids += base::StringPrintf(" %d", pending_id);
        has_pending_changes = true;
      }
    }
    if (has_pending_changes) {
      error_ = base::StringPrintf(
          "Changes left pending by the update; "
          "destroy subtrees: %s, destroy nodes: %s, create nodes: %s",
          destroy_subtree_ids.c_str(), destroy_node_ids.c_str(),
          create_node_ids.c_str());
    }
    return !has_pending_changes;
  }

  return true;
}

void AXTree::MarkSubtreeForDestruction(AXNode::AXID node_id,
                                       AXTreeUpdateState* update_state) {
  update_state->IncrementPendingDestroySubtreeCount(node_id);
  MarkNodesForDestructionRecursive(node_id, update_state);
}

void AXTree::MarkNodesForDestructionRecursive(AXNode::AXID node_id,
                                              AXTreeUpdateState* update_state) {
  // If this subtree has already been marked for destruction, return so
  // we don't walk it again.
  if (!update_state->ShouldPendingNodeExistInTree(node_id))
    return;

  const AXNodeData& last_known_data =
      update_state->GetLastKnownPendingNodeData(node_id);

  update_state->IncrementPendingDestroyNodeCount(node_id);
  for (AXNode::AXID child_id : last_known_data.child_ids) {
    MarkNodesForDestructionRecursive(child_id, update_state);
  }
}

void AXTree::DestroySubtree(AXNode* node,
                            AXTreeUpdateState* update_state) {
  DCHECK(GetTreeUpdateInProgressState());
  // |update_state| must already contain information about all of the expected
  // changes and invalidations to apply. If any of these are missing, observers
  // may not be notified of changes.
  DCHECK(update_state);
  DCHECK_GT(update_state->GetPendingDestroySubtreeCount(node->id()), 0);
  DCHECK(!node->parent() ||
         update_state->InvalidatesUnignoredCachedValues(node->parent()->id()));
  update_state->DecrementPendingDestroySubtreeCount(node->id());
  DestroyNodeAndSubtree(node, update_state);
}

void AXTree::DestroyNodeAndSubtree(AXNode* node,
                                   AXTreeUpdateState* update_state) {
  DCHECK(GetTreeUpdateInProgressState());
  DCHECK(!update_state ||
         update_state->GetPendingDestroyNodeCount(node->id()) > 0);

  // Clear out any reverse relations.
  AXNodeData empty_data;
  empty_data.id = node->id();
  UpdateReverseRelations(node, empty_data);

  // Remove any table infos.
  const auto& table_info_entry = table_info_map_.find(node->id());
  if (table_info_entry != table_info_map_.end()) {
    delete table_info_entry->second;
    table_info_map_.erase(node->id());
  }

  id_map_.erase(node->id());
  for (auto* child : node->children())
    DestroyNodeAndSubtree(child, update_state);
  if (update_state) {
    update_state->pending_nodes.erase(node->id());
    update_state->DecrementPendingDestroyNodeCount(node->id());
    update_state->removed_node_ids.insert(node->id());
    update_state->new_node_ids.erase(node->id());
    update_state->node_data_changed_ids.erase(node->id());
    if (update_state->IsReparentedNode(node)) {
      update_state->old_node_id_to_data.emplace(
          std::make_pair(node->id(), node->TakeData()));
    }
  }
  node->Destroy();
}

void AXTree::DeleteOldChildren(AXNode* node,
                               const std::vector<int32_t>& new_child_ids,
                               AXTreeUpdateState* update_state) {
  DCHECK(GetTreeUpdateInProgressState());
  // Create a set of child ids in |src| for fast lookup, we know the set does
  // not contain duplicate entries already, because that was handled when
  // populating |update_state| with information about all of the expected
  // changes to be applied.
  std::set<int32_t> new_child_id_set(new_child_ids.begin(),
                                     new_child_ids.end());

  // Delete the old children.
  for (AXNode* child : node->children()) {
    if (!base::Contains(new_child_id_set, child->id()))
      DestroySubtree(child, update_state);
  }
}

bool AXTree::CreateNewChildVector(AXNode* node,
                                  const std::vector<int32_t>& new_child_ids,
                                  std::vector<AXNode*>* new_children,
                                  AXTreeUpdateState* update_state) {
  DCHECK(GetTreeUpdateInProgressState());
  bool success = true;
  for (size_t i = 0; i < new_child_ids.size(); ++i) {
    int32_t child_id = new_child_ids[i];
    AXNode* child = GetFromId(child_id);
    if (child) {
      if (child->parent() != node) {
        // This is a serious error - nodes should never be reparented.
        // If this case occurs, continue so this node isn't left in an
        // inconsistent state, but return failure at the end.
        error_ = base::StringPrintf(
            "Node %d reparented from %d to %d",
            child->id(),
            child->parent() ? child->parent()->id() : 0,
            node->id());
        success = false;
        continue;
      }
      child->SetIndexInParent(i);
    } else {
      child = CreateNode(node, child_id, i, update_state);
      update_state->pending_nodes.insert(child->id());
    }
    new_children->push_back(child);
  }

  return success;
}

void AXTree::SetEnableExtraMacNodes(bool enabled) {
  if (enable_extra_mac_nodes_ == enabled)
    return;  // No change.
  if (enable_extra_mac_nodes_ && !enabled) {
    NOTREACHED()
        << "We don't support disabling the extra Mac nodes once enabled.";
    return;
  }

  DCHECK_EQ(0U, table_info_map_.size());
  enable_extra_mac_nodes_ = enabled;
}

int32_t AXTree::GetNextNegativeInternalNodeId() {
  int32_t return_value = next_negative_internal_node_id_;
  next_negative_internal_node_id_--;
  if (next_negative_internal_node_id_ > 0)
    next_negative_internal_node_id_ = -1;
  return return_value;
}

void AXTree::PopulateOrderedSetItems(
    const AXNode& original_node,
    const AXNode* ordered_set,
    std::vector<const AXNode*>& items_to_be_populated) const {
  // Ignored nodes are not a part of ordered sets.
  if (original_node.IsIgnored())
    return;

  // Default hierarchical_level is 0, which represents that no hierarchical
  // level was detected on |original_node|.
  int original_node_min_level = original_node.GetIntAttribute(
      ax::mojom::IntAttribute::kHierarchicalLevel);

  // If we are calling this function on the ordered set container itself, that
  // is |original_node| is ordered set, then set |original_node|'s hierarchical
  // level to be the min level of |original_node|'s direct children, if the
  // child's level is defined.
  if (&original_node == ordered_set) {
    for (AXNode::UnignoredChildIterator itr =
             original_node.UnignoredChildrenBegin();
         itr != original_node.UnignoredChildrenEnd(); ++itr) {
      int32_t child_level =
          itr->GetIntAttribute(ax::mojom::IntAttribute::kHierarchicalLevel);
      if (child_level > 0)
        original_node_min_level =
            original_node_min_level > 0
                ? std::min(child_level, original_node_min_level)
                : child_level;
    }
  }

  RecursivelyPopulateOrderedSetItems(original_node, ordered_set, ordered_set,
                                     original_node_min_level,
                                     items_to_be_populated);
}

void AXTree::RecursivelyPopulateOrderedSetItems(
    const AXNode& original_node,
    const AXNode* ordered_set,
    const AXNode* local_parent,
    int32_t original_node_min_level,
    std::vector<const AXNode*>& items_to_be_populated) const {
  // Stop searching recursively on node |local_parent| if it turns out to be an
  // ordered set whose role matches that of the top level ordered set.
  if (ordered_set->data().role == local_parent->data().role &&
      ordered_set != local_parent)
    return;

  for (AXNode::UnignoredChildIterator itr =
           local_parent->UnignoredChildrenBegin();
       itr != local_parent->UnignoredChildrenEnd(); ++itr) {
    const AXNode* child = itr.get();

    // Invisible children should not be counted.
    // However, in the collapsed container case (e.g. a combobox), items can
    // still be chosen/navigated. However, the options in these collapsed
    // containers are historically marked invisible. Therefore, in that case,
    // count the invisible items. Only check 2 levels up, as combobox containers
    // are never higher.
    if (child->data().HasState(ax::mojom::State::kInvisible) &&
        !IsCollapsed(local_parent) && !IsCollapsed(local_parent->parent())) {
      continue;
    }

    // Add child to |items_to_be_populated| if role matches with the role of
    // |ordered_set|. If role of node is kRadioButton, don't add items of other
    // roles, even if item role matches the role of |ordered_set|.
    if (child->data().role == ax::mojom::Role::kComment ||
        (original_node.data().role == ax::mojom::Role::kRadioButton &&
         child->data().role == ax::mojom::Role::kRadioButton) ||
        (original_node.data().role != ax::mojom::Role::kRadioButton &&
         child->SetRoleMatchesItemRole(ordered_set))) {
      int child_level =
          child->GetIntAttribute(ax::mojom::IntAttribute::kHierarchicalLevel);

      // If the hierarchical level of |child| and the level of |original_node|
      // differ, we do not add child to |items_to_be_populated| and we do not
      // recurse into |child| and populate its order set item descendants.
      // Additionally, as an exception, we always add tab items to the set,
      // because according to WAI-ARIA spec, tab does not support hierarchical
      // level, while tab's set container tablist supports hierarchical level.
      // Due to this, we always assume sibling tabs are always on the same
      // level, and always add tab child item to |items_to_be_populated|.
      // https://www.w3.org/WAI/PF/aria/roles#tab
      // https://www.w3.org/WAI/PF/aria/roles#tablist
      if (child_level != original_node_min_level &&
          child->data().role != ax::mojom::Role::kTab) {
        if (child_level < original_node_min_level &&
            original_node.GetUnignoredParent() == child->GetUnignoredParent()) {
          // For a flattened structure, where |original_node| and |child| share
          // the same parent, if a decrease in level occurs after
          // |original_node| has been examined (i.e. |original_node|'s index
          // comes before that of |child|), we stop adding to this set, and stop
          // from populating |child|'s other siblings to |items_to_be_populated|
          // as well.
          if (original_node.GetUnignoredIndexInParent() <
              child->GetUnignoredIndexInParent())
            break;

          // For a flattened structure, where |original_node| and |child| share
          // the same parent, if a decrease in level has been detected before
          // |original_node| has been examined (i.e. |original_node|'s index
          // comes after that of |child|), then everything previously added to
          // items actually belongs to a different set. Clear the items set.
          items_to_be_populated.clear();
        }
        continue;
      }

      // We only add child to |items_to_be_populated| if the child set item is
      // at the same hierarchical level as |original_node|'s level.
      items_to_be_populated.push_back(child);
    }

    // Recurse if there is a generic container, ignored, or unknown.
    if (child->IsIgnored() ||
        child->data().role == ax::mojom::Role::kGenericContainer ||
        child->data().role == ax::mojom::Role::kUnknown) {
      RecursivelyPopulateOrderedSetItems(original_node, ordered_set, child,
                                         original_node_min_level,
                                         items_to_be_populated);
    }
  }
}

// Given an ordered_set, compute pos_in_set and set_size for all of its items
// and store values in cache.
// Ordered_set should never be nullptr.
void AXTree::ComputeSetSizePosInSetAndCache(const AXNode& node,
                                            const AXNode* ordered_set) {
  DCHECK(ordered_set);
  std::vector<const AXNode*> items;
  // Find all items within ordered_set and add to vector.
  PopulateOrderedSetItems(node, ordered_set, items);

  // If ordered_set role is kPopUpButton and it wraps a kMenuListPopUp, then we
  // would like it to inherit the SetSize from the kMenuListPopUp it wraps. To
  // do this, we treat the kMenuListPopUp as the ordered_set and eventually
  // assign its SetSize value to the kPopUpButton.
  if ((node.data().role == ax::mojom::Role::kPopUpButton) &&
      (items.size() != 0)) {
    // kPopUpButtons are only allowed to contain one kMenuListPopUp.
    // The single element is guaranteed to be a kMenuListPopUp because that is
    // the only item role that matches the ordered set role of kPopUpButton.
    // Please see AXNode::SetRoleMatchesItemRole for more details.
    DCHECK(items.size() == 1);
    const AXNode* menu_list_popup = items[0];
    items.clear();
    PopulateOrderedSetItems(node, menu_list_popup, items);
  }

  // Keep track of the number of elements ordered_set has.
  int32_t num_elements = 0;
  // Necessary for calculating set_size.
  int32_t largest_assigned_set_size = 0;

  // Compute pos_in_set_values.
  for (size_t i = 0; i < items.size(); ++i) {
    const AXNode* item = items[i];
    ordered_set_info_map_[item->id()] = OrderedSetInfo();
    int32_t pos_in_set_value = 0;
    int hierarchical_level =
        item->GetIntAttribute(ax::mojom::IntAttribute::kHierarchicalLevel);

    pos_in_set_value = num_elements + 1;

    // Check if item has a valid kPosInSet assignment, which takes precedence
    // over previous assignment. Invalid assignments are decreasing or
    // duplicates, and should be ignored.
    pos_in_set_value =
        std::max(pos_in_set_value,
                 item->GetIntAttribute(ax::mojom::IntAttribute::kPosInSet));

    // If level is specified, use author-provided value, if present.
    if (hierarchical_level != 0 &&
        item->HasIntAttribute(ax::mojom::IntAttribute::kPosInSet)) {
      pos_in_set_value =
          item->GetIntAttribute(ax::mojom::IntAttribute::kPosInSet);
    }

    // Assign pos_in_set and update role counts.
    ordered_set_info_map_[item->id()].pos_in_set = pos_in_set_value;
    num_elements = pos_in_set_value;

    // Check if kSetSize is assigned and update if it's the largest assigned
    // kSetSize.
    if (item->HasIntAttribute(ax::mojom::IntAttribute::kSetSize))
      largest_assigned_set_size =
          std::max(largest_assigned_set_size,
                   item->GetIntAttribute(ax::mojom::IntAttribute::kSetSize));
  }

  // Compute set_size value.
  // The SetSize of an ordered set (and all of its items) is the maximum of the
  // following candidate values:
  // 1. The number of elements in the ordered set.
  // 2. The Largest assigned SetSize in the ordered set.
  // 3. The SetSize assigned within the ordered set.

  // Set to 0 if ordered_set has no kSetSize attribute.
  int32_t ordered_set_candidate =
      ordered_set->GetIntAttribute(ax::mojom::IntAttribute::kSetSize);

  int32_t set_size_value = std::max(
      std::max(num_elements, largest_assigned_set_size), ordered_set_candidate);

  // Assign set_size to ordered_set.
  // Must meet one of two conditions:
  // 1. Node role matches ordered set role.
  // 2. The node that calculations were called on is the ordered_set.
  if (node.SetRoleMatchesItemRole(ordered_set) || ordered_set == &node) {
    auto ordered_set_info_result =
        ordered_set_info_map_.find(ordered_set->id());
    int hierarchical_level =
        node.GetIntAttribute(ax::mojom::IntAttribute::kHierarchicalLevel);
    // If ordered_set is not in the cache, assign it a new set_size.
    if (ordered_set_info_result == ordered_set_info_map_.end()) {
      ordered_set_info_map_[ordered_set->id()] = OrderedSetInfo();
      ordered_set_info_map_[ordered_set->id()].set_size = set_size_value;
      ordered_set_info_map_[ordered_set->id()].lowest_hierarchical_level =
          hierarchical_level;
    } else {
      OrderedSetInfo ordered_set_info = ordered_set_info_result->second;
      if (ordered_set_info.lowest_hierarchical_level > hierarchical_level) {
        ordered_set_info.set_size = set_size_value;
        ordered_set_info.lowest_hierarchical_level = hierarchical_level;
      }
    }
  }

  // Assign set_size to items.
  for (size_t j = 0; j < items.size(); ++j) {
    const AXNode* item = items[j];
    int hierarchical_level =
        item->GetIntAttribute(ax::mojom::IntAttribute::kHierarchicalLevel);
    // If level is specified, use author-provided value, if present.
    if (hierarchical_level != 0 &&
        item->HasIntAttribute(ax::mojom::IntAttribute::kSetSize))
      ordered_set_info_map_[item->id()].set_size =
          item->GetIntAttribute(ax::mojom::IntAttribute::kSetSize);
    else
      ordered_set_info_map_[item->id()].set_size = set_size_value;
  }
}

// Returns the pos_in_set of item. Looks in ordered_set_info_map_ for cached
// value. Calculates pos_in_set and set_size for item (and all other items in
// the same ordered set) if no value is present in the cache.
// This function is guaranteed to be only called on nodes that can hold
// pos_in_set values, minimizing the size of the cache.
int32_t AXTree::GetPosInSet(const AXNode& node, const AXNode* ordered_set) {
  // If item's id is not in the cache, compute it.
  if (ordered_set_info_map_.find(node.id()) == ordered_set_info_map_.end())
    ComputeSetSizePosInSetAndCache(node, ordered_set);
  return ordered_set_info_map_[node.id()].pos_in_set;
}

// Returns the set_size of node. node could be an ordered set or an item.
// Looks in ordered_set_info_map_ for cached value. Calculates pos_inset_set
// and set_size for all nodes in same ordered set if no value is present in the
// cache.
// This function is guaranteed to be only called on nodes that can hold
// set_size values, minimizing the size of the cache.
int32_t AXTree::GetSetSize(const AXNode& node, const AXNode* ordered_set) {
  // If node's id is not in the cache, compute it.
  if (ordered_set_info_map_.find(node.id()) == ordered_set_info_map_.end())
    ComputeSetSizePosInSetAndCache(node, ordered_set);
  return ordered_set_info_map_[node.id()].set_size;
}

AXTree::Selection AXTree::GetUnignoredSelection() const {
  Selection unignored_selection = {
      data().sel_is_backward,     data().sel_anchor_object_id,
      data().sel_anchor_offset,   data().sel_anchor_affinity,
      data().sel_focus_object_id, data().sel_focus_offset,
      data().sel_focus_affinity};
  AXNode* anchor_node = GetFromId(data().sel_anchor_object_id);
  AXNode* focus_node = GetFromId(data().sel_focus_object_id);

  AXNodePosition::AXPositionInstance anchor_position =
      anchor_node ? AXNodePosition::CreatePosition(*anchor_node,
                                                   data().sel_anchor_offset,
                                                   data().sel_anchor_affinity)
                  : AXNodePosition::CreateNullPosition();

  // Null positions are never ignored.
  if (anchor_position->IsIgnored()) {
    anchor_position = anchor_position->AsUnignoredPosition(
        data().sel_is_backward ? AXPositionAdjustmentBehavior::kMoveForwards
                               : AXPositionAdjustmentBehavior::kMoveBackwards);

    // Any selection endpoint that is inside a leaf node is expressed as a text
    // position in AXTreeData.
    if (anchor_position->IsLeafTreePosition())
      anchor_position = anchor_position->AsTextPosition();

    // We do not expect the selection to have an endpoint on an inline text
    // box as this will create issues with parts of the code that don't use
    // inline text boxes.
    if (anchor_position->IsTextPosition() &&
        anchor_position->GetAnchor()->data().role ==
            ax::mojom::Role::kInlineTextBox) {
      anchor_position = anchor_position->CreateParentPosition();
    }

    switch (anchor_position->kind()) {
      case AXPositionKind::NULL_POSITION:
        // If one of the selection endpoints is invalid, then both endpoints
        // should be unset.
        unignored_selection.anchor_object_id = AXNode::kInvalidAXID;
        unignored_selection.anchor_offset = -1;
        unignored_selection.anchor_affinity =
            ax::mojom::TextAffinity::kDownstream;
        unignored_selection.focus_object_id = AXNode::kInvalidAXID;
        unignored_selection.focus_offset = -1;
        unignored_selection.focus_affinity =
            ax::mojom::TextAffinity::kDownstream;
        return unignored_selection;
      case AXPositionKind::TREE_POSITION:
        unignored_selection.anchor_object_id = anchor_position->anchor_id();
        unignored_selection.anchor_offset = anchor_position->child_index();
        unignored_selection.anchor_affinity =
            ax::mojom::TextAffinity::kDownstream;
        break;
      case AXPositionKind::TEXT_POSITION:
        unignored_selection.anchor_object_id = anchor_position->anchor_id();
        unignored_selection.anchor_offset = anchor_position->text_offset();
        unignored_selection.anchor_affinity = anchor_position->affinity();
        break;
    }
  }

  AXNodePosition::AXPositionInstance focus_position =
      focus_node
          ? AXNodePosition::CreatePosition(*focus_node, data().sel_focus_offset,
                                           data().sel_focus_affinity)
          : AXNodePosition::CreateNullPosition();

  // Null positions are never ignored.
  if (focus_position->IsIgnored()) {
    focus_position = focus_position->AsUnignoredPosition(
        !data().sel_is_backward ? AXPositionAdjustmentBehavior::kMoveForwards
                                : AXPositionAdjustmentBehavior::kMoveBackwards);

    // Any selection endpoint that is inside a leaf node is expressed as a text
    // position in AXTreeData.
    if (focus_position->IsLeafTreePosition())
      focus_position = focus_position->AsTextPosition();

    // We do not expect the selection to have an endpoint on an inline text
    // box as this will create issues with parts of the code that don't use
    // inline text boxes.
    if (focus_position->IsTextPosition() &&
        focus_position->GetAnchor()->data().role ==
            ax::mojom::Role::kInlineTextBox) {
      focus_position = focus_position->CreateParentPosition();
    }

    switch (focus_position->kind()) {
      case AXPositionKind::NULL_POSITION:
        // If one of the selection endpoints is invalid, then both endpoints
        // should be unset.
        unignored_selection.anchor_object_id = AXNode::kInvalidAXID;
        unignored_selection.anchor_offset = -1;
        unignored_selection.anchor_affinity =
            ax::mojom::TextAffinity::kDownstream;
        unignored_selection.focus_object_id = AXNode::kInvalidAXID;
        unignored_selection.focus_offset = -1;
        unignored_selection.focus_affinity =
            ax::mojom::TextAffinity::kDownstream;
        return unignored_selection;
      case AXPositionKind::TREE_POSITION:
        unignored_selection.focus_object_id = focus_position->anchor_id();
        unignored_selection.focus_offset = focus_position->child_index();
        unignored_selection.focus_affinity =
            ax::mojom::TextAffinity::kDownstream;
        break;
      case AXPositionKind::TEXT_POSITION:
        unignored_selection.focus_object_id = focus_position->anchor_id();
        unignored_selection.focus_offset = focus_position->text_offset();
        unignored_selection.focus_affinity = focus_position->affinity();
        break;
    }
  }

  return unignored_selection;
}

bool AXTree::GetTreeUpdateInProgressState() const {
  return tree_update_in_progress_;
}

void AXTree::SetTreeUpdateInProgressState(bool set_tree_update_value) {
  tree_update_in_progress_ = set_tree_update_value;
}

bool AXTree::HasPaginationSupport() const {
  return has_pagination_support_;
}

}  // namespace ui
