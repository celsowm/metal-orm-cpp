#pragma once

#include "metal/runtime_types.hpp"
#include "metal/unit_of_work.hpp"

#include <unordered_set>

namespace metal {

class RelationChangeProcessor {
public:
    explicit RelationChangeProcessor(UnitOfWork& unit_of_work)
        : unit_of_work_(unit_of_work) {}

    // Cascaded persist must happen before the first UoW flush so generated
    // keys exist before relation DML is compiled.
    void prepare() {
        std::unordered_set<void*> prepared;
        while (prepared.size() < unit_of_work_.keys().size()) {
            const auto keys = unit_of_work_.keys();
            bool progressed = false;
            for (void* key : keys) {
                if (prepared.contains(key)) continue;
                auto* tracked = unit_of_work_.find(key);
                if (!tracked) continue;
                prepared.insert(key);
                progressed = true;
                if (tracked->prepare_relations) tracked->prepare_relations();
            }
            if (!progressed) break;
        }
    }

    // Mirrors MetalORM TS flush order: entity UoW first, relation changes,
    // then another UoW flush for removals/updates scheduled by relations.
    void process() {
        for (void* key : unit_of_work_.keys()) {
            auto* tracked = unit_of_work_.find(key);
            if (tracked && tracked->flush_relations) tracked->flush_relations();
        }
    }

    void accept() {
        for (void* key : unit_of_work_.keys()) {
            auto* tracked = unit_of_work_.find(key);
            if (tracked && tracked->accept_relations) tracked->accept_relations();
        }
    }

    void reset() {}

private:
    UnitOfWork& unit_of_work_;
};

} // namespace metal
