#pragma once

#include <QTreeView>
#include <QHash>
#include <QPersistentModelIndex>
#include <QTimer>

#include "Core/Snowflake.hpp"
#include "UI/GuildSettings/GuildSettingsSection.hpp"

namespace Acheron {
namespace UI {

class MentionJumpIndicator;

class ChannelTreeView : public QTreeView
{
    Q_OBJECT
public:
    ChannelTreeView(QWidget *parent = nullptr);

    void setModel(QAbstractItemModel *m) override;
    void performDefaultExpansion();

    void setAccountVoiceChannel(Core::Snowflake accountId, Core::Snowflake channelId);
    [[nodiscard]] bool isAccountInVoice(Core::Snowflake accountId) const;
    void setGuildSettingsProvider(GuildSettingsAccess::SectionsProvider provider);
    void setMentionJumpEnabled(bool enabled);

signals:
    void markAsReadRequested(const QModelIndex &proxyIndex);
    void openInNewTabRequested(const QModelIndex &proxyIndex);
    void joinVoiceChannelRequested(const QModelIndex &proxyIndex);
    void disconnectVoiceRequested(const QModelIndex &proxyIndex);
    void joinThreadRequested(const QModelIndex &proxyIndex);
    void leaveThreadRequested(const QModelIndex &proxyIndex);
    void leaveGuildRequested(Core::Snowflake accountId, Core::Snowflake guildId);
    void guildSettingsRequested(Core::Snowflake accountId, Core::Snowflake guildId, GuildSettingsSection section);
    void voiceParticipantContextMenuRequested(const QModelIndex &proxyIndex, QPoint globalPos);
    // A live participant's row has been hovered long enough for its stream thumbnail to be shown;
    // an invalid index means the hover ended.
    void liveParticipantHoverChanged(const QModelIndex &proxyIndex);
    // A live participant's row was double-clicked, which is how the official client watches.
    void watchStreamRequested(const QModelIndex &proxyIndex);

protected:
    void contextMenuEvent(QContextMenuEvent *event) override;

private:
    void mousePressEvent(QMouseEvent *event) override;
    void mouseDoubleClickEvent(QMouseEvent *event) override;
    void mouseMoveEvent(QMouseEvent *event) override;
    void leaveEvent(QEvent *event) override;

    void setHoveredLiveParticipant(const QModelIndex &proxyIndex);

    bool handleMouseEventForExpansion(QMouseEvent *event);

    void onRowsInserted(const QModelIndex &parent, int first, int last);

    Core::Snowflake findAccountIdForIndex(const QModelIndex &sourceIndex) const;

    [[nodiscard]] bool rowHasUnshownMentions(const QModelIndex &row) const;

    QHash<Core::Snowflake, Core::Snowflake> accountVoiceChannels; // accountId -> channelId
    MentionJumpIndicator *mentionJump = nullptr;

    QTimer liveHoverDelay;
    QPersistentModelIndex pendingLiveHover;
    QPersistentModelIndex shownLiveHover;
    GuildSettingsAccess::SectionsProvider guildSettingsProvider;
};

} // namespace UI
} // namespace Acheron
