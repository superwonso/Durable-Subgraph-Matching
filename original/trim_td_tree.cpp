#include <iostream>
#include <vector>
#include <set>
#include <unordered_set>
#include <algorithm>
#include "utils.h"
using namespace std;

// Placeholder functions for non_edge_edge_test and remove_block
bool non_edge_edge_test(int v, TreeNode* u) {
    TreeNode* current = u;
    while (current != nullptr) {
        if (current->bloom_filter->possiblyContains(to_string(v))) {
            return false;
        }
        current = current->parent;
    }
    return true;
}

void remove_block(TreeNode* b, TreeNode* u);

// Function prototypes
void preorder_traversal(TreeNode* node, void (*func)(TreeNode*));
void postorder_traversal(TreeNode* node, void (*func)(TreeNode*));

// Algorithm 2: TrimTDTree
Tree* TrimTDTree(Tree* T) {
    // Preorder traversal
    preorder_traversal(T->root, [](TreeNode* u) {
        for (auto b : u->children) {
            for (int v : b->V_cand) {
                // C is the set of nodes that contain the blocks pointed to by the pointers associated with v
                set<TreeNode*> C;
                for (auto child : u->children) {
                    if (find(child->V_cand.begin(), child->V_cand.end(), v) != child->V_cand.end()) {
                        C.insert(child);
                    }
                }

                if (C.size() < u->children.size() && !non_edge_edge_test(v, u)) {
                    for (auto b_prime : u->children) {
                        if (b_prime->V_cand.count(v)) {
                            remove_block(b_prime, u);
                        }
                    }
                    b->V_cand.erase(v);
                }

                if (b->V_cand.empty()) {
                    u->children.erase(remove(u->children.begin(), u->children.end(), b), u->children.end());
                }
            }
        }
    });

    // Postorder traversal
    postorder_traversal(T->root, [](TreeNode* u) {
        for (auto b : u->children) {
            for (int v : b->V_cand) {
                // C is the set of nodes that contain the blocks pointed to by the pointers associated with v
                set<TreeNode*> C;
                for (auto child : u->children) {
                    if (find(child->V_cand.begin(), child->V_cand.end(), v) != child->V_cand.end()) {
                        C.insert(child);
                    }
                }

                if (C.size() < u->children.size() && !non_edge_edge_test(v, u)) {
                    for (auto b_prime : u->children) {
                        if (b_prime->V_cand.count(v)) {
                            remove_block(b_prime, u);
                        }
                    }
                    b->V_cand.erase(v);
                }

                if (b->V_cand.empty()) {
                    u->children.erase(remove(u->children.begin(), u->children.end(), b), u->children.end());
                }
            }
        }
    });

    return T;
}

// Preorder traversal
void preorder_traversal(TreeNode* node, void (*func)(TreeNode*)) {
    if (!node) return;
    func(node);
    for (auto child : node->children) {
        preorder_traversal(child, func);
    }
}

// Postorder traversal
void postorder_traversal(TreeNode* node, void (*func)(TreeNode*)) {
    if (!node) return;
    for (auto child : node->children) {
        postorder_traversal(child, func);
    }
    func(node);
}

// remove_block function
void remove_block(TreeNode* b, TreeNode* u) {
    if (find(u->children.begin(), u->children.end(), b) != u->children.end()) {
        for (int v : b->V_cand) {
            for (auto b_prime : u->children) {
                if (b_prime->V_cand.count(v)) {
                    remove_block(b_prime, u);
                }
            }
        }
        u->children.erase(remove(u->children.begin(), u->children.end(), b), u->children.end());
    }
}

int main() {
    // Example usage
    Tree* T = new Tree;

    Tree* result = TrimTDTree(T);

    // Further processing with result

    return 0;
}
