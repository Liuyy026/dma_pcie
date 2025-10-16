#include "mylistwidget.h"
#include <QContextMenuEvent>

MyListWidget::MyListWidget(QWidget *parent)
    : QListWidget(parent), m_rowHeight(20)
{
    // 创建右键菜单
    m_contextMenu = new QMenu(this);

    // 添加菜单项
    m_deleteAction = m_contextMenu->addAction(tr("删除"));
    m_moveUpAction = m_contextMenu->addAction(tr("向上移动"));
    m_moveDownAction = m_contextMenu->addAction(tr("向下移动"));

    // 连接信号和槽
    connect(m_deleteAction, &QAction::triggered, this, &MyListWidget::onDeleteItem);
    connect(m_moveUpAction, &QAction::triggered, this, &MyListWidget::onMoveItemUp);
    connect(m_moveDownAction, &QAction::triggered, this, &MyListWidget::onMoveItemDown);

    // 设置选择模式
    setSelectionMode(QAbstractItemView::SingleSelection);
}

MyListWidget::~MyListWidget()
{
    delete m_contextMenu;
}

void MyListWidget::setRowHeight(int height)
{
    m_rowHeight = height;

    // 更新所有项的大小提示
    for (int i = 0; i < count(); ++i)
    {
        item(i)->setSizeHint(QSize(width(), m_rowHeight));
    }
}

bool MyListWidget::moveSelectedItemUp()
{
    int rowIndex = currentRow();
    if (rowIndex > 0)
    {
        QListWidgetItem *currentItem = takeItem(rowIndex);
        insertItem(rowIndex - 1, currentItem);
        setCurrentItem(currentItem);
        return true;
    }
    return false;
}

bool MyListWidget::moveSelectedItemDown()
{
    int rowIndex = currentRow();
    if (rowIndex < count() - 1 && rowIndex >= 0)
    {
        QListWidgetItem *currentItem = takeItem(rowIndex);
        insertItem(rowIndex + 1, currentItem);
        setCurrentItem(currentItem);
        return true;
    }
    return false;
}

bool MyListWidget::deleteSelectedItem()
{
    int rowIndex = currentRow();
    if (rowIndex >= 0)
    {
        delete takeItem(rowIndex);
        return true;
    }
    return false;
}

void MyListWidget::clearAll()
{
    clear();
}

void MyListWidget::contextMenuEvent(QContextMenuEvent *event)
{
    // 只有在有选中项时才显示右键菜单
    if (currentRow() >= 0)
    {
        // 根据当前位置启用或禁用菜单项
        m_moveUpAction->setEnabled(currentRow() > 0);
        m_moveDownAction->setEnabled(currentRow() < count() - 1);

        // 显示右键菜单
        m_contextMenu->exec(event->globalPos());
    }
}

void MyListWidget::onDeleteItem()
{
    deleteSelectedItem();
}

void MyListWidget::onMoveItemUp()
{
    moveSelectedItemUp();
}

void MyListWidget::onMoveItemDown()
{
    moveSelectedItemDown();
}