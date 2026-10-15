// SPDX-License-Identifier: GPL-3.0-or-later
// Project document -> immutable engine snapshots (control thread).
#pragma once

#include <functional>
#include <string>

#include "daw/engine.hpp"
#include "daw/model.hpp"

namespace daw {

// engine instrument slot of a channel, -1 when it has none
using SlotOf = std::function<int(Id channel)>;
// resolves an automation target to engine terms (plugin parameter ids and ranges); false = skip
using ResolveTarget = std::function<bool(const AutomationTarget &, PlayAuto &)>;

PlayModel *compilePlay(const Project &p, const SlotOf &slotOf, const ResolveTarget &resolve, uint32_t version);
void freePlay(const PlayModel *m);

// nullptr + err when the routing has a cycle
MixModel *compileMix(const Project &p, std::string *err = nullptr);
void freeMix(const MixModel *m);

}  // namespace daw
