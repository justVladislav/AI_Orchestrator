#pragma once

#include "ai/application/conversations/ChatTypes.hpp"
#include "ai/application/ProjectContextSnapshot.hpp"
#include "ai/common/Result.hpp"

#include <string>
#include <string_view>

namespace ai::application {

// The boundary token of one attachment: a domain-separated digest of the snapshot fingerprint. Section text is
// authored before the fingerprint exists (and the fingerprint covers that text), so text inside a section cannot
// contain the token of its own snapshot. Only markers that carry this token are real.
[[nodiscard]] std::string chatProjectContextBoundaryToken(std::string_view snapshotFingerprint);

// True for a line that imitates a STRUCTURAL line of the rendering: a section marker, the begin/end headings, the
// security paragraph or the boundary field (case-insensitive, leading blanks ignored). Such a line inside a section
// is quoted with a leading backslash when rendered. Ordinary data lines (for example the workspace identity) are not.
[[nodiscard]] bool isChatProjectContextControlLikeLine(std::string_view line);

// Provider-facing rendering of a PC-1 snapshot. Every section's text is DATA: it is preserved line by line except
// that a line which imitates a control line is quoted with a leading backslash, and the real markers carry the
// attachment's boundary token, so untrusted text cannot close a section, open an "authoritative" one or end the
// snapshot early. Work Ledger prose and derived Project Memory are additionally labelled untrusted.
[[nodiscard]] std::string renderProjectContextSnapshotForChat(const ProjectContextSnapshot& snapshot);

// Seals one already-built PC-1 snapshot for one Assistant row. Production snapshots must use the fixed v1 budget
// policy. Whether derived Project Memory is included is a property of the SNAPSHOT (a Project Memory section is
// present), never of what the user asked for: a requested memory that is empty, or was squeezed out by the budget,
// simply is not in the snapshot. The snapshot must also pass validateProjectContextSnapshotIntegrity (its fingerprint is
// recomputed from its content), so what is sealed is exactly what the fingerprint names.
// Failure codes: chat_context.invalid_snapshot / invalid_identity / invalid_attachment.
[[nodiscard]] common::Result<ChatProjectContextAttachment> makeChatProjectContextAttachment(
    const ChatMessageId& assistantMessageId,
    const ConversationId& conversationId,
    const ProjectContextSnapshot& snapshot);

// Shared fail-closed validation used at the Application and persistence boundaries.
[[nodiscard]] common::Result<void> validateChatProjectContextAttachment(
    const ChatProjectContextAttachment& attachment);

} // namespace ai::application
