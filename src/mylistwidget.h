#ifndef MYLISTWIDGET_H
#define MYLISTWIDGET_H

#include <QListWidget>
#include <QMenu>

class MyListWidget : public QListWidget
{
    Q_OBJECT

public:
    explicit MyListWidget(QWidget *parent = nullptr);
    ~MyListWidget();

    // 设置行高
    void setRowHeight(int height);

    // 移动选中项向上或向下
    bool moveSelectedItemUp();
    bool moveSelectedItemDown();

    // 删除选中项
    bool deleteSelectedItem();

    // 清空列表
    void clearAll();

protected:
    // 右键菜单
    void contextMenuEvent(QContextMenuEvent *event) override;

private slots:
    void onDeleteItem();
    void onMoveItemUp();
    void onMoveItemDown();

private:
    QMenu *m_contextMenu;
    QAction *m_deleteAction;
    QAction *m_moveUpAction;
    QAction *m_moveDownAction;

    int m_rowHeight;
};

#endif // MYLISTWIDGET_H