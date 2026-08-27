#include "../header/widget.h"
#include "ui_Widget.h"
#include "utils/Util.h"
#include <QDebug>
#include <QWindow>
#include <QScreen>
#include "utils/setWindowBlur.h"
#include "utils/IconOnlyDelegate.h"
#include <QPainter>
#include <QPen>
#include <QDateTime>
#include <QSet>
#include "utils/QtWin.h"
#include <QWheelEvent>
#include <QTimer>
#include <QMetaEnum>
#include "utils/SystemTray.h"
#include "utils/ConfigManager.h"
#include <QVBoxLayout>

class WindowListPopup : public QWidget {
public:
    QListWidget* lw;
    WindowListPopup(QWidget* parent) : QWidget(parent, Qt::Tool | Qt::FramelessWindowHint | Qt::WindowStaysOnTopHint) {
        setAttribute(Qt::WA_TranslucentBackground);
        lw = new QListWidget(this);
        lw->setStyleSheet(R"(
            QListWidget {
                background-color: transparent;
                border: none;
                outline: none;
                color: rgb(240, 240, 240);
                font-family: "Microsoft YaHei UI", "Microsoft YaHei", "Consolas";
            }
            QListWidget::item {
                padding: 6px 10px;
                border-left: 3px solid transparent;
                border-radius: 4px;
            }
            QListWidget::item:selected {
                background-color: rgba(255, 255, 255, 40);
                border-left: 3px solid #ffffff;
            }
        )");
        lw->setVerticalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
        lw->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
        lw->setAutoScroll(false); // Prevent the ListWidget from trying to scroll to "make item visible"
        auto layout = new QVBoxLayout(this);
        layout->addWidget(lw);
        layout->setContentsMargins(8, 8, 8, 8);
    }
    void paintEvent(QPaintEvent*) override {
        QPainter painter(this);
        painter.setRenderHint(QPainter::Antialiasing);
        painter.setPen(Qt::NoPen);
        painter.setBrush(QColor(25, 25, 25, 100));
        painter.drawRect(rect());
    }
    void showEvent(QShowEvent* e) override {
        QWidget::showEvent(e);
        Util::setWindowRoundCorner((HWND)winId());
        setWindowBlur((HWND)winId());
    }
};

Widget::Widget(QWidget* parent) : QWidget(parent), ui(new Ui::Widget) {
    ui->setupUi(this);
    lw = ui->listWidget;
    setWindowFlag(Qt::WindowStaysOnTopHint);
    setWindowFlag(Qt::FramelessWindowHint);
    setAttribute(Qt::WA_TranslucentBackground); //设置窗口背景透明 !但是会造成show()时的闪烁 和 绘制延迟(?)
    QtWin::taskbarDeleteTab(this); //删除任务栏图标
    setWindowTitle("AltTaber");

    Util::setWindowRoundCorner(this->hWnd()); // 设置窗口圆角
    setWindowBlur(hWnd()); // 设置窗口模糊, 必须配合Qt::WA_TranslucentBackground

    popup = new WindowListPopup(this);
    popup->hide();

    setupLabelFont();

    lw->setViewMode(QListView::IconMode);
    lw->setMovement(QListView::Static);
    lw->setFlow(QListView::LeftToRight);
    lw->setWrapping(true); // rows are capped to the screen width in prepareListWidget
    lw->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    lw->setVerticalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    lw->setIconSize({64, 64});
    lw->setGridSize({80, 80});
    lw->setFixedHeight(lw->gridSize().height());
    lw->setUniformItemSizes(true); // optimization ?
    lw->setStyleSheet(R"(
        QListWidget {
            background-color: transparent;
            border: none;
            outline: none; /* 去除选中时的虚线框（在文字为空时，会形成闪电一样的标志 离谱） */
        }
    )");
    // 就算Text为Null，也会占用空间，很难做到真正的IConMode，所以只能delegate自绘
    // 本来为了去除图标选中变色样式，可以对Icon手动addPixmap(..., QIcon::Selected) or (& ~Qt::ItemIsSelectable)
    // 但是采用delegate后，就没必要了
    // will not take ownership of delegate
    lw->setItemDelegate(new IconOnlyDelegate(lw));
    lw->installEventFilter(this);

    connect(lw, &QListWidget::currentItemChanged, this, [this](QListWidgetItem* cur, QListWidgetItem* prev) {
        // Only clear when changing selection interactively between items (prevents initial load clear)
        if (cur && prev && cur != prev) {
            groupWindowOrder.clear(); 
        }
        if (cur) showLabelForItem(cur);
    });

    connect(qApp, &QApplication::focusWindowChanged, this, [this](QWindow* focusWindow) {
        if (focusWindow == nullptr) {
            if (!this->underMouse()) { // hide when lost focus & mouse outside (means user choose to)
                hide();
                if (popup) popup->hide();
            } else { // Windows Terminal will do
                qWarning() << "Someone tried to steal focus!";
            }
        }
    });
}

Widget::~Widget() {
    delete ui;
    if (popup) popup->deleteLater();
}

void Widget::keyPressEvent(QKeyEvent* event) {
    auto key = event->key();
    auto modifiers = event->modifiers();
    static const QHash<int, int> VimArrows = {
        {Qt::Key_K, Qt::Key_Up},    // ↑
        {Qt::Key_J, Qt::Key_Down},  // ↓
        {Qt::Key_H, Qt::Key_Left},  // ←
        {Qt::Key_L, Qt::Key_Right}, // →
    };
    if (key == Qt::Key_Tab) { // switch to next or prev
        groupWindowOrder.clear(); // Ensure we cycle the correct app when changing apps
        auto i = lw->currentRow();
        bool isShiftPressed = (modifiers & Qt::ShiftModifier);
        // weird formula, but works (hhh)
        auto index = (i - (2 * isShiftPressed - 1) + lw->count()) % lw->count();
        lw->setCurrentRow(index);
    } else if (key == Qt::Key_QuoteLeft && (modifiers & Qt::AltModifier)) { // Alt + `, 在前台窗口同组窗口内切换
        if (this->isVisible() && !this->isMinimized()) {
            if (auto item = lw->currentItem()) {
                auto windowGroup = item->data(Qt::UserRole).value<WindowGroup>();
                if (groupWindowOrder.isEmpty())
                    groupWindowOrder = buildGroupWindowOrder(windowGroup.key);
                
                HWND currentHwnd = nullptr;
                if (popup && popup->isVisible() && popup->lw->currentItem()) {
                    currentHwnd = reinterpret_cast<HWND>(popup->lw->currentItem()->data(Qt::UserRole).value<void*>());
                }
                
                HWND nextHwnd = nullptr;
                if (currentHwnd && groupWindowOrder.contains(currentHwnd)) {
                    nextHwnd = rotateWindowInGroup(groupWindowOrder, currentHwnd, !(modifiers & Qt::ShiftModifier));
                } else {
                    nextHwnd = groupWindowOrder.value(1, groupWindowOrder.value(0)); // skip foreWin if possible
                }
                
                if (nextHwnd) {
                    Util::bringWindowToTop(nextHwnd, this->hWnd());
                    showLabelForItem(item, Util::getWindowTitle(nextHwnd), nextHwnd);
                }
            }
            return;
        }
        
        auto foreWin = GetForegroundWindow();
        if (groupWindowOrder.isEmpty()) {
            auto groupKey = getWindowGroupKey(foreWin);
            groupWindowOrder = buildGroupWindowOrder(groupKey);
        }
        
        HWND nextWin = rotateWindowInGroup(groupWindowOrder, foreWin, !(modifiers & Qt::ShiftModifier));
        if (!nextWin) {
            nextWin = groupWindowOrder.value(1, groupWindowOrder.value(0));
        }
        
        if (nextWin) {
            Util::bringWindowToTop(nextWin, this->hWnd());
            this->requestShowForCurrentApp();
            
            if (auto item = lw->currentItem()) {
                showLabelForItem(item, Util::getWindowTitle(nextWin), nextWin);
            }
        }
    } else if (key == Qt::Key_Up || key == Qt::Key_Down) {
        if (auto item = lw->currentItem()) {
            auto center = lw->visualItemRect(item).center();
            // 转发映射到WheelEvent
            auto wheelEvent = new QWheelEvent(center, lw->mapToGlobal(center), {},
                                              {key == Qt::Key_Up ? 120 : -120, 0},
                                              Qt::NoButton, Qt::NoModifier, Qt::NoScrollPhase, false);
            QApplication::postEvent(lw, wheelEvent);
        }
    } else if (key == Qt::Key_Left || key == Qt::Key_Right) { // 默认情况下 左右键可以切换item 只需要处理边界循环即可
        groupWindowOrder.clear(); // Ensure we cycle the correct app when changing apps
        const int N = lw->count();
        const int i = lw->currentRow();
        if (key == Qt::Key_Left && i == 0)
            lw->setCurrentRow(N - 1);
        else if (key == Qt::Key_Right && i == N - 1)
            lw->setCurrentRow(0);
    } else if (VimArrows.contains(key)) { // map [K J H L] to [↑ ↓ ← →]
        QApplication::postEvent(lw, new QKeyEvent(QEvent::KeyPress, VimArrows.value(key), modifiers));
    }
    QWidget::keyPressEvent(event);
}

bool Widget::forceShow() {
    setWindowOpacity(0.005); // 减少闪烁发生(因Qt::WA_TranslucentBackground) in showMinimized()
    showMinimized();
    showNormal();
    setWindowOpacity(1);
    return isForeground();
}

/// show App description under the icon
void Widget::showLabelForItem(QListWidgetItem* item, QString text, HWND focusHwnd) {
    if (!item) return;

    bool isWindowRotation = !text.isNull();

    if (text.isNull()) {
        auto group = item->data(Qt::UserRole).value<WindowGroup>();
        text = group.name.isEmpty() ? Util::getFileDescription(group.exePath) : group.name;
    }
    ui->label->setText(text);
    ui->label->adjustSize();

    auto itemRect = lw->visualItemRect(item);
    // below the whole grid, not just the item's row, so the label never covers the row underneath
    auto center = QPoint(itemRect.center().x(), lw->height() + ListWidgetMargin.bottom() / 2);
    center = lw->mapTo(this, center);
    auto labelRect = ui->label->rect();
    labelRect.moveCenter(center);

    auto bound = this->rect().marginsRemoved({5, 0, 5, 0});
    labelRect.moveRight(qMin(labelRect.right(), bound.right()));
    labelRect.moveLeft(qMax(labelRect.left(), bound.left())); // left align

    ui->label->move(labelRect.topLeft());

    // Update the window list popup
    if (popup) {
        auto group = item->data(Qt::UserRole).value<WindowGroup>();
        if (group.windows.size() > 1) {
            popup->lw->clear();
            QFont font = ui->label->font();
            popup->lw->setFont(font);

            int maxWidth = 200; // minimum width
            bool itemSelected = false;

            if (groupWindowOrder.isEmpty()) {
                groupWindowOrder = buildGroupWindowOrder(group.key);
            }
            
            // Build the list items matching the verified groupWindowOrder order which correctly reflects valid active windows
            for (const HWND& hwnd : groupWindowOrder) {
                QString listText = Util::getWindowTitle(hwnd);
                if (listText.isEmpty()) listText = Util::getClassName(hwnd);
                
                auto winItem = new QListWidgetItem(listText);
                winItem->setData(Qt::UserRole, QVariant::fromValue(reinterpret_cast<void*>(hwnd)));
                popup->lw->addItem(winItem);
                
                QFontMetrics fm(font);
                int textWidth = fm.horizontalAdvance(listText) + 32; // padding
                maxWidth = qMax(maxWidth, textWidth);

                // Check if this window is currently selected/active
                if (focusHwnd && hwnd == focusHwnd) {
                    popup->lw->setCurrentItem(winItem);
                    itemSelected = true;
                } else if (!focusHwnd && isWindowRotation && listText == text) {
                    popup->lw->setCurrentItem(winItem);
                    itemSelected = true;
                }
            }
            
            if (!itemSelected && popup->lw->count() > 0) {
                // Either not rotating or we didn't find a match. Select the target window.
                HWND targetHwnd = nullptr;
                if (!groupWindowOrder.isEmpty()) {
                    targetHwnd = groupWindowOrder.first();
                }
                
                if (targetHwnd) {
                    for (int i = 0; i < popup->lw->count(); i++) {
                        auto wItem = popup->lw->item(i);
                        auto h = reinterpret_cast<HWND>(wItem->data(Qt::UserRole).value<void*>());
                        if (h == targetHwnd) {
                            popup->lw->setCurrentItem(wItem);
                            break;
                        }
                    }
                }
            }
            
            // Limit width
            maxWidth = qMin(maxWidth, 800);
            
            // Calculate accurate height based on item size and margins
            int itemHeight = 31; // slightly overestimating the height per item to prevent 1px scroll triggering
            int calculatedHeight = (popup->lw->count() * itemHeight) + 18; // margins + tiny padding cushion
            int popupHeight = qMin(calculatedHeight, 400); 
            popup->resize(maxWidth, popupHeight);
            
            // Position popup dynamically below the currently highlighted App Icon
            int iconCenterX = lw->mapTo(this, itemRect.center()).x();
            int localX = iconCenterX - popup->width() / 2;
            
            // Prevent the popup from drawing outside the left/right bounds of the main AltTaber bar
            if (localX < 0) localX = 0;
            if (localX + popup->width() > this->width()) {
                localX = this->width() - popup->width();
            }
            
            QPoint globalPos = this->mapToGlobal(QPoint(localX, this->height() + 10));
            popup->move(globalPos);
            popup->show();
            popup->raise();
        } else {
            popup->hide();
        }
    }
}

void Widget::setupLabelFont() {
    static auto reloadLabelFontCfg = [this] {
        const QStringList Fonts = {"Microsoft YaHei UI", "Microsoft YaHei", "Consolas"}; // fallback
        auto labelFont = ui->label->font();
        labelFont.setPointSize(cfg.get("label/font_size", 10).toInt());
        auto defaultFF = QStringList{cfg.get("label/font_family", Fonts[0]).toString()};
        labelFont.setFamilies(defaultFF << Fonts.mid(1));
        ui->label->setFont(labelFont);
        qDebug() << labelFont.families();
        qDebug() << "Label Actual Font:" << QFontInfo(labelFont).family();
    };
    reloadLabelFontCfg();

    // auto reload
    connect(&cfg, &ConfigManager::configEdited, this, [] {
        reloadLabelFontCfg();
    });
}

void Widget::keyReleaseEvent(QKeyEvent* event) {
    if (event->key() == Qt::Key_Alt) {
        groupWindowOrder.clear(); // for Alt + `
        if (this->isVisible()) {
            // active selected window
            if (auto item = lw->currentItem()) {
                if (auto group = item->data(Qt::UserRole).value<WindowGroup>(); !group.windows.empty()) {
                    HWND targetHwnd = nullptr;
                    if (popup && popup->isVisible() && popup->lw->currentItem()) {
                        targetHwnd = reinterpret_cast<HWND>(popup->lw->currentItem()->data(Qt::UserRole).value<void*>());
                    } else {
                        WindowInfo targetWin = group.windows.at(0); // TODO 需要排序（lastActiveWindow 被关闭情况下）
                        const auto lastActive = getLastActiveGroupWindow(group.key).first;
                        for (auto& info: group.windows) {
                            if (info.hwnd == lastActive) {
                                targetWin = info;
                                break;
                            }
                        }
                        targetHwnd = targetWin.hwnd;
                    }
                    if (targetHwnd) {
                        auto run = recentGroupRun(buildGroupWindowOrder(group.key), group.key);
                        for (int i = run.size() - 1; i >= 0; --i) {
                            HWND sib = run.at(i);
                            if (sib != targetHwnd && !IsIconic(sib)) {
                                Util::bringWindowToTop(sib, this->hWnd());
                                // Only the target gets a real OS focus event below; without logging the
                                // raised siblings too, a later tab-away-and-back would see them as stale
                                // and collapse the run to a single window. Re-log the cluster contiguously.
                                recordFocusEvent(group.key, sib);
                            }
                        }
                        recordFocusEvent(group.key, targetHwnd); // target is newest in the cluster
                        Util::switchToWindow(targetHwnd);
                        qInfo() << "Switch to" << targetHwnd << group.key;
                    }
                }
            }
            hide(); //! must hide after active target window, or focus may fallback to prev foreground window (like 网易云音乐)
            if (popup) popup->hide();
        }
    }
    QWidget::keyReleaseEvent(event);
}

void Widget::paintEvent(QPaintEvent*) { //不绘制会导致鼠标穿透背景
    QPainter painter(this);
    painter.setRenderHint(QPainter::Antialiasing);
    painter.setPen(Qt::NoPen); //取消边框//pen决定边框颜色
    painter.setBrush(QColor(25, 25, 25, 100));
    painter.drawRect(rect());
}

/// Windows already tells us which taskbar button a window belongs to, which is what separates
/// Chrome/Edge profiles and PWAs sharing one browser process. Apps without one group by exe.
QString Widget::getWindowGroupKey(HWND hwnd) {
    if (auto appId = Util::getWindowAppId(hwnd); !appId.isEmpty()) return appId;
    return Util::getWindowProcessPath(hwnd);
}

/// Record `hwnd` (and its group) as active at `now`, promoting the whole app near the top of MRU.
/// Preserves relative order within the group: focused window = now, others = now - 1ms, -2ms, ...
void Widget::recordWindowActive(HWND hwnd, const QString& groupKey, const QDateTime& now) {
    winActiveOrder[groupKey].insert(hwnd, now);
    hwndActiveTime.insert(hwnd, now);
    recordFocusEvent(groupKey, hwnd); // genuine focus of this exact window; siblings below are NOT logged

    auto& groupOrder = winActiveOrder[groupKey];
    QList<QPair<HWND, QDateTime>> siblings;
    for (auto it = groupOrder.begin(); it != groupOrder.end(); ++it) {
        if (it.key() != hwnd)
            siblings.append({it.key(), it.value()});
    }
    if (siblings.isEmpty()) return;
    // Sort siblings by their existing timestamp (most recent first), then re-stamp just behind `hwnd`
    std::sort(siblings.begin(), siblings.end(), [](const auto& a, const auto& b) {
        return a.second > b.second;
    });
    for (int i = 0; i < siblings.size(); ++i) {
        groupOrder.insert(siblings[i].first, now.addMSecs(-(i + 1)));
        hwndActiveTime.insert(siblings[i].first, now.addMSecs(-(i + 1)));
    }
}

void Widget::notifyForegroundChanged(HWND hwnd, ForegroundChangeSource source, int retries) { // TODO isVisible or AltDown时，关闭前台更新通知
    if (hwnd == this->hWnd()) return;

    // A window isn't always ready the instant its foreground event fires: a freshly-launched or
    // slow-starting window often has no title / unresolved exe path yet, so it isn't "acceptable"
    // or groupable. Dropping it here is fatal — once it's already foreground, no further event
    // fires, so it's never recorded and sinks to the bottom of the MRU list. Retry instead.
    bool ready = Util::isWindowAcceptable(hwnd, source == WinEvent);
    QString groupKey = ready ? getWindowGroupKey(hwnd) : QString();
    if (!ready || groupKey.isEmpty()) {
        if (source == WinEvent && retries > 0) {
            QTimer::singleShot(300, this, [this, hwnd, retries]() {
                if (IsWindow(hwnd) && hwnd == GetForegroundWindow())
                    notifyForegroundChanged(hwnd, WinEvent, retries - 1);
            });
        } else if (ready && IsWindow(hwnd)) {
            // Acceptable but still ungroupable after retries (e.g. UWP core window never attached):
            // keep a flat HWND timestamp so getLastValidActiveGroupWindow's fallback can order it.
            hwndActiveTime.insert(hwnd, QDateTime::currentDateTime());
            recordFocusEvent(QString(), hwnd); // empty group = treated as a boundary in the history
        }
        return;
    }

    recordWindowActive(hwnd, groupKey, QDateTime::currentDateTime());

    auto sourceStr = QMetaEnum::fromType<ForegroundChangeSource>().valueToKey(source);
    qDebug() << qUtf8Printable(QString("*ForeWin changed (%1):").arg(sourceStr))
            << Util::getWindowTitle(hwnd) << Util::getClassName(hwnd) << groupKey;
} // TODO 控制面板 和 资源管理器 exe是同一个，如何区分图标

/// Stamp the *current* foreground window as active now, so the app you're using (or just launched)
/// is always at the top of the MRU — even if its EVENT_SYSTEM_FOREGROUND was missed because the
/// window had no title yet when it fired. This is the authoritative correction at display time.
void Widget::captureForegroundActive() {
    HWND foreWin = GetForegroundWindow();
    if (!foreWin || foreWin == this->hWnd()) return;

    // Resolve to the listed (top-level, acceptable) window: usually the foreground window itself,
    // but for owned dialogs (e.g. Unity's "Package Manager") it's the owner that appears in the list.
    HWND target = foreWin;
    while (target && !Util::isWindowAcceptable(target, false))
        target = GetWindow(target, GW_OWNER);
    if (!target) return;

    auto groupKey = getWindowGroupKey(target);
    if (groupKey.isEmpty()) return;
    recordWindowActive(target, groupKey, QDateTime::currentDateTime());
}

/// collect, filter, sort Windows for presentation
QList<WindowGroup> Widget::prepareWindowGroupList() {
    // Assert the current foreground app as most-recently-active before sorting, so it's always on
    // top even if its foreground event was missed (slow launch / no title at event time).
    captureForegroundActive();

    QMap<QString, WindowGroup> winGroupMap;
    const auto list = Util::listValidWindows();
    for (auto hwnd: list) {
        if (hwnd == this->hWnd()) continue; // skip self
        auto groupKey = getWindowGroupKey(hwnd);
        if (groupKey.isEmpty()) continue; // TODO 可能需要管理员权限
        auto& winGroup = winGroupMap[groupKey];
        if (winGroup.key.isEmpty()) {
            winGroup.key = groupKey;
            winGroup.exePath = Util::getWindowProcessPath(hwnd);
            winGroup.name = Util::getWindowAppName(hwnd);
            winGroup.icon = Util::getResourceIcon(Util::getWindowAppIconResource(hwnd)); // e.g. per-profile Chrome icon
        }
        winGroup.addWindow({Util::getWindowTitle(hwnd), Util::getClassName(hwnd), hwnd});
    }

    QHash<QString, int> identitiesPerExe;
    for (auto& group: winGroupMap)
        identitiesPerExe[group.exePath.toLower()]++;

    for (auto& group: winGroupMap) {
        if (!group.icon.isNull()) continue;
        HWND hwnd = group.windows.first().hwnd;
        if (identitiesPerExe.value(group.exePath.toLower()) > 1) {
            // One exe behind several taskbar buttons (PWAs): its icon can't tell them apart, so use the window's own
            group.icon = QIcon(Util::getWindowIcon(hwnd));
            if (group.name.isEmpty()) group.name = group.windows.first().title;
            continue;
        }
        auto icon = Util::getCachedIcon(group.exePath, hwnd); // TODO background thread
        if (group.exePath.endsWith("QQ\\bin\\QQ.exe", Qt::CaseInsensitive)) { // draw chat partner for classical QQ
            QPixmap overlay = Util::getWindowIcon(hwnd);
            const auto iSize = lw->iconSize();
            QPixmap bgPixmap = icon.pixmap(iSize);
            icon = Util::overlayIcon(bgPixmap, overlay, {{iSize.width() / 2, iSize.height() / 2}, iSize / 2});
        }
        group.icon = icon;
    }

    auto winGroupList = winGroupMap.values();
    // 按照活跃度排序
    std::sort(winGroupList.begin(), winGroupList.end(), [this](const WindowGroup& a, const WindowGroup& b) {
        auto timeA = getLastValidActiveGroupWindow(a).second;
        auto timeB = getLastValidActiveGroupWindow(b).second;
        if (timeA.isNull() && timeB.isNull()) return false;
        if (timeA.isValid() && timeB.isValid()) return timeA > timeB;
        return timeA.isValid();
    });
    return winGroupList;
}

bool Widget::prepareListWidget(bool selectCurrentApp) {
    auto winGroupList = prepareWindowGroupList();
    lw->clear();
    for (auto& winGroup: winGroupList) {
        auto item = new QListWidgetItem(winGroup.icon, {}); // null != "", which will completely hide text area
        item->setData(Qt::UserRole, QVariant::fromValue(winGroup));
        item->setSizeHint(lw->gridSize()); // 决定了delegate的绘制区域，比grid小的话，paintRect就不居中了，而且update也不及时
//        item->setFlags(item->flags() & ~Qt::ItemIsSelectable); // 不可选中
        lw->addItem(item);
    }

    // calculate Geometry
    if (auto firstItem = lw->item(0)) {
        // get screen
        bool displayOnPrimary = (cfg.getDisplayMonitor() == PrimaryMonitor);
        auto screen = displayOnPrimary ?
                      QGuiApplication::primaryScreen() :
                      QGuiApplication::screenAt(QCursor::pos()); // multi-screen support
        if (!screen && !displayOnPrimary) { // fallback to primary screen
            qWarning() << "Cursor Screen nullptr! Fallback to primary";
            screen = QApplication::primaryScreen();
        }
        if (!screen) {
            qWarning() << "Screen nullptr!";
            sysTray.showMessage("Error", "Screen nullptr!");
            return false;
        }

        // wrap the icons into as many rows as needed to stay inside the screen
        const auto grid = lw->gridSize();
        const int pad = lw->visualItemRect(firstItem).x() - lw->frameWidth(); // 一些微小的噼里啪啦修正
        const int maxWidth = screen->availableGeometry().width() - ListWidgetMargin.left() - ListWidgetMargin.right();
        const int columns = qBound(1, (maxWidth - pad) / grid.width(), lw->count());
        lw->setFixedWidth(grid.width() * columns + pad);
        lw->doItemsLayout(); // so the row count below reflects the new width
        lw->setFixedHeight(lw->visualItemRect(lw->item(lw->count() - 1)).bottom() + 1 + pad);

        // move to scrren center
        qDebug() << "Screen:" << screen->name();
        auto lwRect = lw->rect();
        auto thisRect = lwRect.marginsAdded(ListWidgetMargin);
        thisRect.moveCenter(screen->geometry().center());

        //region Fixed in Qt6, see commit [b927ee4b]
        //      !!!WARNING: 对于多屏幕，直接使用setGeometry or move会报错(QWindowsWindow::setGeometry: Unable to set geometry) & size显示不正确！
        //      报错时机为：从一个屏幕hide，再在另一个屏幕show; 第二次在同一个屏幕show，则正常
        //      size显示不正确不能忍，遂改用WinAPI
        //endregion
        this->setGeometry(thisRect);

        //region Fixed in Qt6, see commit [b927ee4b]
        //this->windowHandle()->setScreen(screen); // 若首次显示是在副屏，会导致size显示错误（如果没有这行）
        // `toNativePixels`是针对Point的，会根据屏幕原点进行位移
        // 对于其他类型（如Size），直接乘以`QHighDpiScaling::factor(screen)`即可
        //auto physicalPos = QHighDpi::toNativePixels(thisRect.topLeft(), screen);
        // 如果用SetWindowPos的话要注意加上`SWP_NOACTIVATE`，否则焦点有问题，没错，NoActive反而是Active (focus)的
        //SetWindowPos(hWnd(), nullptr, physicalPos.x(), physicalPos.y(), 0, 0, SWP_NOACTIVATE | SWP_NOSIZE | SWP_NOZORDER);
        // !!!NOTE: 用WinAPI控制size貌似有问题，在图标增减的时候，无法正确调整Width，离子谱；只能用resize
        // 1. × 猜想是showNormal()恢复了原有的size，导致resize无效；但是改成show()也不行
        // 2. 猜想是隐藏状态改变size无效？（不科学吧），但是在`SetWindowPos`前show()好像会好一点（第二次显示调整size成功）aaa
        //this->resize(thisRect.size());
        //endregion

        lwRect.moveCenter(this->rect().center()); // local pos
        lw->move(lwRect.topLeft());
    } else {
        // no item, hide ? TODO
        return false;
    }

    // set current item
    if (lw->count() >= 2) {
        auto foreWin = GetForegroundWindow();
        bool isFirstItemForeground = false;
        for (auto& info: winGroupList.at(0).windows) {
            if (info.hwnd == foreWin) {
                isFirstItemForeground = true;
                break;
            }
        }
        // If the foreground window wasn't found, walk the owner chain.
        // Owned windows (e.g., Unity's "Package Manager") are filtered out by isWindowAcceptable
        // but their owner (the main app window) is in the list.
        if (!isFirstItemForeground) {
            HWND owner = foreWin;
            while ((owner = GetWindow(owner, GW_OWNER)) != nullptr) {
                for (auto& info: winGroupList.at(0).windows) {
                    if (info.hwnd == owner) {
                        isFirstItemForeground = true;
                        break;
                    }
                }
                if (isFirstItemForeground) break;
            }
        }

        qDebug() << "ForeWin:" << foreWin << Util::getWindowTitle(foreWin)
                 << "owner:" << GetWindow(foreWin, GW_OWNER)
                 << "isFirstItemFore:" << isFirstItemForeground
                 << "row:" << (selectCurrentApp ? 0 : (isFirstItemForeground ? 1 : 0));

        // 如果第一个item是前台窗口，就选中第二个
        // 因为有些情况：选中桌面 并不会产生一个item
        if (selectCurrentApp) {
            lw->setCurrentRow(0);
        } else {
            lw->setCurrentRow(isFirstItemForeground ? 1 : 0); //! 首次显示时，该行特别耗时：472ms
        }
    } else if (lw->count() == 1) {
        lw->setCurrentRow(0);
    }

    return true;
}

bool Widget::requestShow() { // TODO 当前台是开始菜单（Win）时，会导致显示 但无法操控
    return prepareListWidget(false) && forceShow();
}

bool Widget::requestShowForCurrentApp() {
    return prepareListWidget(true) && forceShow();
}

/// Warning: the `HWND` not guarantee to be valid (may be closed)
auto Widget::getLastActiveGroupWindow(const QString& groupKey) -> QPair<HWND, QDateTime> {
    auto hwndOrder = winActiveOrder.value(groupKey);
    if (hwndOrder.isEmpty()) return {nullptr, QDateTime()};
    // QHash & QMap deref to value(QDateTime) rather than QPair
    auto iter = std::max_element(hwndOrder.begin(), hwndOrder.end());
    return {iter.key(), iter.value()};
}

/// return null if no window recorded in group
auto Widget::getLastValidActiveGroupWindow(const WindowGroup& group) -> QPair<HWND, QDateTime> {
    auto hwndOrder = winActiveOrder.value(group.key);

    QList<HWND> windows;
    for (auto& info: group.windows)
        windows << info.hwnd;
    sortGroupWindows(windows, group.key);

    if (!hwndOrder.isEmpty()) {
        if (auto time = hwndOrder.value(windows.first()); !time.isNull())
            return {windows.first(), time};
    }

    // Fallback: search hwndActiveTime directly by HWND.
    // Handles a window whose groupKey was never recorded, e.g. UWP on first launch when
    // getAppCoreWindow() still fails.
    HWND bestHwnd = nullptr;
    QDateTime bestTime;
    for (auto& info : group.windows) {
        auto t = hwndActiveTime.value(info.hwnd);
        if (!t.isNull() && (bestTime.isNull() || t > bestTime)) {
            bestTime = t;
            bestHwnd = info.hwnd;
        }
    }
    return {bestHwnd, bestTime};
}

/// sort Windows of [Group specified by groupKey], by active order (latest first)
void Widget::sortGroupWindows(QList<HWND>& windows, const QString& groupKey) {
    auto activeOrdMap = winActiveOrder.value(groupKey);
    // sort by active order, falling back to hwndActiveTime when groupKey-based entry is missing
    std::sort(windows.begin(), windows.end(), [&](HWND a, HWND b) {
        bool aMin = IsIconic(a);
        bool bMin = IsIconic(b);
        if (aMin != bMin) return !aMin; // minimized windows sort last
        auto timeA = activeOrdMap.value(a);
        auto timeB = activeOrdMap.value(b);
        if (timeA.isNull()) timeA = hwndActiveTime.value(a);
        if (timeB.isNull()) timeB = hwndActiveTime.value(b);
        return timeA > timeB;
    }); // TODO update winActiveOrder! (remove invalid HWND)
}

/// Append a genuine focus event to the history, collapsing consecutive duplicates and capping length.
void Widget::recordFocusEvent(const QString& groupKey, HWND hwnd) {
    if (!focusHistory.isEmpty() && focusHistory.last().second == hwnd) {
        focusHistory.last().first = groupKey; // refresh groupKey (e.g. PWA title settled), keep one entry
        return;
    }
    focusHistory.append({groupKey, hwnd});
    constexpr int MaxHistory = 256;
    if (focusHistory.size() > MaxHistory)
        focusHistory.remove(0, focusHistory.size() - MaxHistory);
}

/// Of the group's windows (sorted newest-active first), keep only the group's most-recent *contiguous
/// run* in the real focus timeline — i.e. drop windows that sit below an intervening foreign-app focus.
/// This stops a freshly-touched app from dragging up siblings you haven't looked at in ages.
QList<HWND> Widget::recentGroupRun(const QList<HWND>& siblings, const QString& groupKey) {
    if (siblings.size() <= 1) return siblings;

    const QSet<HWND> groupSet(siblings.begin(), siblings.end());

    // Find the group's most recent focus in the history; without one we have nothing to anchor against.
    int last = -1;
    for (int i = focusHistory.size() - 1; i >= 0; --i) {
        if (focusHistory.at(i).first == groupKey) { last = i; break; }
    }
    if (last < 0) return siblings; // group never recorded → preserve raise-all behavior

    // Walk backwards from there, collecting still-open group windows until a *foreign* focus interrupts.
    // Same-group entries for already-closed windows don't break the run (they're just skipped).
    QSet<HWND> runSet;
    for (int i = last; i >= 0; --i) {
        const auto& ev = focusHistory.at(i);
        if (ev.first != groupKey) break; // a different app was focused here → end of the recent run
        if (groupSet.contains(ev.second)) runSet.insert(ev.second);
    }

    QList<HWND> run;
    for (HWND h : siblings)
        if (runSet.contains(h)) run << h;
    if (run.isEmpty()) run << siblings.first(); // safety: always keep the newest
    return run;
}

/// group by groupKey, sort by active order (last active first)
QList<HWND> Widget::buildGroupWindowOrder(const QString& groupKey) {
    QList<HWND> windows;
    auto allWindows = Util::listValidWindows();
    for (auto hwnd : allWindows) {
        if (getWindowGroupKey(hwnd) == groupKey) {
            windows << hwnd;
        }
    }
    sortGroupWindows(windows, groupKey);
    return windows;
}

bool Widget::eventFilter(QObject* watched, QEvent* event) {
    if (watched == lw && event->type() == QEvent::Wheel) {
        auto* wheelEvent = static_cast<QWheelEvent*>(event);
        auto cursorPos = wheelEvent->position().toPoint();
        if (auto item = lw->itemAt(cursorPos)) {
            if (lw->currentItem() != item)
                lw->setCurrentItem(item);
            auto windowGroup = item->data(Qt::UserRole).value<WindowGroup>();
            if (windowGroup.windows.isEmpty()) return false;

            static QListWidgetItem* lastItem = nullptr;
            static HWND hwnd = nullptr;
            if (lastItem != item) { // Alt+Tab也可能造成切换; 每次show列表都是重新构建，所以item指针必然不同（即使同一个app）
                lastItem = item;
                hwnd = nullptr;
                groupWindowOrder.clear();
            }
            auto targetExe = windowGroup.key;
            static bool isLastRollUp = true;
            bool isRollUp = wheelEvent->angleDelta().x() > 0; // ListWidget的方向改成了从左到右，所以滚轮方向从y()变成x()了
            if (groupWindowOrder.isEmpty())
                groupWindowOrder = buildGroupWindowOrder(targetExe); // TODO 其实这里不需要build 直接用lw里的就行...

            if (!hwnd) { // first time
                hwnd = groupWindowOrder.first(); // 选择最后活跃的窗口 TODO 考虑当前窗口就是First的情况，需要跳过，类似WinGroup
            } else { // select next window
                if (isLastRollUp == isRollUp) // 滚轮方向切换时，不轮换窗口
                    hwnd = rotateWindowInGroup(groupWindowOrder, hwnd, isRollUp);
            }
            isLastRollUp = isRollUp;

            HWND nextFocus = hwnd; // this隐藏后的焦点备选窗口, for `swtichToWindow` after AltUp
            if (isRollUp) {
                Util::bringWindowToTop(hwnd, this->hWnd()); // without activate
            } else {
                if (auto normal = rotateNormalWindowInGroup(groupWindowOrder, hwnd, false)) { // skip minimized
                    ShowWindow(normal, SW_SHOWMINNOACTIVE); // minimize
                    hwnd = normal;
                    nextFocus = hwnd;
                }
                if (auto normal = rotateNormalWindowInGroup(groupWindowOrder, hwnd, false))
                    nextFocus = normal; // 备选焦点切换为下一个非最小化窗口 after AltUp
            }
            notifyForegroundChanged(nextFocus, Inner);
            showLabelForItem(item, Util::getWindowTitle(nextFocus));
            qDebug() << "Wheel" << isRollUp << Util::getWindowTitle(nextFocus) << hwnd;

            return true; // stop propagation
        }
    }
    return false;
}

/// `forward`: true for restore, false for minimize
void Widget::rotateTaskbarWindowInGroup(const QString& appId, const QString& exePath, bool forward, int windows) {
    qDebug() << "(Taskbar)Wheel on:" << appId << exePath << forward << windows;
    if (exePath.isEmpty() && appId.isEmpty()) return;
    if (!windows) { // 程序没有打开的窗口，处于关闭状态; 若不拦截，可能造成错误窗口被触发：explorer.exe -> msedge.exe
        qDebug() << "No window for this app";
        return;
    }

    // The taskbar button's appId is the same identity the windows are stamped with, so it selects
    // the right Chrome profile / PWA; apps whose windows carry none are still grouped by exe.
    static QString lastKey;
    static HWND lastHwnd = nullptr;
    if (lastKey != appId + exePath) {
        lastKey = appId + exePath;
        groupWindowOrder.clear();
    }
    if (groupWindowOrder.isEmpty()) {
        groupWindowOrder = buildGroupWindowOrder(appId);
        if (groupWindowOrder.isEmpty())
            groupWindowOrder = buildGroupWindowOrder(exePath);
        lastHwnd = nullptr;
    }

    if (groupWindowOrder.isEmpty()) {
        qCritical() << "No window in group!" << exePath;
        // 有些软件的窗口是由子进程创建的，如 steam.exe -> steamwebhelper.exe (持有窗口)
        // 但是在任务栏只能获取到父进程steam.exe
        // 这种情况下，需要查找其子进程的路径
        auto childPaths = Util::getChildProcessPaths(exePath);
        if (childPaths.isEmpty()) return;
        if (childPaths.size() == 1) {
            qDebug() << "Try to switch to child process:" << childPaths.first();
            groupWindowOrder = buildGroupWindowOrder(childPaths.first());
        } else {
            // 如果有多个子进程路径，就根据validWindows过滤
            qWarning() << "!Multiple child processes:" << childPaths;
            QSet<QString> validPaths;
            // If range-initializer returns a temporary, its lifetime is extended until the end of the loop
            for (auto hwnd: Util::listValidWindows()) {
                if (auto path = Util::getWindowProcessPath(hwnd); !path.isEmpty())
                    validPaths.insert(path.toLower());
            }
            for (auto& path: childPaths) {
                if (validPaths.contains(path.toLower())) {
                    qDebug() << "Try to switch to valid child process:" << path;
                    groupWindowOrder = buildGroupWindowOrder(path);
                    break;
                }
            }
        }
        // TODO 有可能a进程开启b进程之后，a就关闭了，他俩也没有真的父子关系
        //  例如：ksolaunch.exe -> wps.exe
        //  此时只能通过File Description来匹配，均为“WPS Office”
        if (groupWindowOrder.isEmpty()) { // 无力回天
            qCritical() << "もうおしまいだ！";
            return;
        }
    }

    static bool isLastForward = true;
    HWND hwnd = nullptr;
    if (!lastHwnd) {
        hwnd = groupWindowOrder.first();
        if (forward && hwnd == GetForegroundWindow()) // 如果first是前台窗口且forward，则轮换下一个
            hwnd = rotateWindowInGroup(groupWindowOrder, hwnd, true);
    } else {
        if (isLastForward == forward)
            hwnd = rotateWindowInGroup(groupWindowOrder, lastHwnd, forward);
        else
            hwnd = lastHwnd;
    }
    isLastForward = forward;

    if (forward) {
        static auto mouseEvent = [](DWORD flag) {
            mouse_event(flag, 0, 0, 0, 0);
        };
        if (windows == 1) { // 由于过滤的存在，groupWindowOrder.size() 不一定等于 windows(真实窗口数量)
            // 单窗口情况下，模拟点击呼出，是最保险的
            if ((hwnd != GetForegroundWindow() || IsIconic(hwnd))) { // 若采用SW_SHOWMINNOACTIVE, 则前台窗口不会变化，可能为刚刚最小化的窗口
                mouseEvent(MOUSEEVENTF_LEFTDOWN);
                mouseEvent(MOUSEEVENTF_LEFTUP);
                qApp->processEvents();
                qDebug() << "(Taskbar)Switch by click";
            }
        } else {
            // 在TaskListThumbnailWnd显示的情况下restore window会导致预览实时刷新，导致卡顿和闪烁
            // 隐藏TaskListThumbnailWnd也无效，会自动show
            // DwmSetWindowAttribute[DWMWA_FORCE_ICONIC_REPRESENTATION, DWMWA_DISALLOW_PEEK], 效果都不好，还是会刷新闪烁

            // 只能采用偷鸡hack，按住左键的情况下，预览窗口会消失
            if (HWND thumbnail = Util::getCurrentTaskListThumbnailWnd(); IsWindowVisible(thumbnail)) {
                qDebug() << "(Taskbar)#Press LButton";
                mouseEvent(MOUSEEVENTF_LEFTDOWN);
                // 由于本程序hook了mouse，所以必须处理全局鼠标事件（in事件循环）
                QTimer::singleShot(20, this, [hwnd]() {
                    Util::switchToWindow(hwnd, true); // TODO thumbnail隐藏之前 不要switch，并且block滚轮 防止闪烁卡顿
                });
            } else
                Util::switchToWindow(hwnd, true);

            static QTimer* timer = [this]() {
                auto* timer = new QTimer;
                timer->setSingleShot(true);
                timer->setInterval(200);
                // TODO cursor移动后立即释放 防止拖拽
                timer->callOnTimeout(this, [this]() {
                    mouseEvent(MOUSEEVENTF_LEFTUP);
                    qDebug() << "(Taskbar)#Release LButton";

                    // 鼠标点击thumbnail之后，其获取焦点，此时若焦点在其窗口组成员中，thumbnail就不会隐藏，这是Windows机制
                    // 只能通过将焦点转移到Taskbar使其隐藏
                    // 直接 HIDE thumbnail 不太行，会导致之后restore窗口时 thumbnail刷新 + 窗口闪烁，闪瞎了
                    QTimer::singleShot(100, this, []() {
                        // 等待thumbnail显示
                        if (HWND thumbnail = Util::getCurrentTaskListThumbnailWnd(); IsWindowVisible(thumbnail)) {
                            if (HWND taskbar = FindWindow(L"Shell_TrayWnd", nullptr))
                                Util::switchToWindow(taskbar, true);
                        }
                    });
                });
                return timer;
            }();
            timer->stop();
            timer->start();
        }
        qDebug() << "(Taskbar)Switch to" << hwnd << Util::getWindowTitle(hwnd) << Util::getClassName(hwnd);
    } else {
        if (auto normal = rotateNormalWindowInGroup(groupWindowOrder, hwnd, false)) { // skip minimized
            if (normal != hwnd)
                qDebug() << "(Taskbar)Skip minimized" << hwnd << "->" << normal;
            hwnd = normal;
            ShowWindow(hwnd, SW_MINIMIZE); // SW_MINIMIZE 会让焦点自动回落到下一个窗口
            // 当所有窗口隐藏后，getElementUnderMouse() 会变成"CEF-OSC-WIDGET"，但是焦点和前台窗口并不是他，离谱
            // 此时Automation对鼠标下任务栏Element的判定会出错，solution为手动变焦到任务栏（见TaskbarWheelHooker.cpp）
            // SW_SHOWMINNOACTIVE不会切换焦点，即便本窗口已经最小化，但仍然持有焦点；但这不是合理的行为，同时会让QQ Follower反复弹出
            qDebug() << "(Taskbar)Minimize" << hwnd << Util::getWindowTitle(hwnd) << Util::getClassName(hwnd);
        }
    }

    lastHwnd = hwnd;
}

/// select next(forward)(older) or prev window in group<br>
/// Do nothing, but select HWND
HWND Widget::rotateWindowInGroup(const QList<HWND>& windows, HWND current, bool forward) {
    const auto N = windows.size();
    if (N == 1) return windows.first();
    for (int i = 0; i < N; i++) {
        if (windows.at(i) == current) {
            auto next_i = forward ? (i + 1) : (i - 1);
            auto next = windows.at((next_i + N) % N);
            return next;
        }
    }
    return nullptr;
}

/// Select next (including `current`) normal (!minimized) window in group<br>
/// return nullptr if all minimized
HWND Widget::rotateNormalWindowInGroup(const QList<HWND>& windows, HWND current, bool forward) {
    for (int i = 0; IsIconic(current) && i < windows.size(); i++) // skip minimized
        current = rotateWindowInGroup(windows, current, forward);
    return IsIconic(current) ? nullptr : current;
}

void Widget::clearGroupWindowOrder() {
    groupWindowOrder.clear();
}
