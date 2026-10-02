#pragma once

#include <QDialog>

class HotwordsEditor : public QDialog {
    Q_OBJECT

public:
    explicit HotwordsEditor(QWidget* parent = nullptr);
    ~HotwordsEditor();
};