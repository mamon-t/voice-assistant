#include "ui/RawParamsDialog.h"

#include <QComboBox>
#include <QDialogButtonBox>
#include <QFileInfo>
#include <QFormLayout>
#include <QLabel>
#include <QPushButton>
#include <QVBoxLayout>

RawParamsDialog::RawParamsDialog(const AudioFileDecoder::RawParams& initial,
                                 const QString& filePath,
                                 QWidget* parent)
    : QDialog(parent)
{
    setWindowTitle(tr("Параметры RAW-аудио"));

    auto* layout = new QVBoxLayout(this);

    auto* hint = new QLabel(
        tr("Файл «%1» не имеет заголовка: частоту, число каналов и кодирование "
           "нельзя прочитать из файла.\n"
           "Телефонные записи обычно: 8000 Гц, моно, 16 бит или G.711 (A-law/μ-law).")
            .arg(QFileInfo(filePath).fileName()),
        this);
    hint->setWordWrap(true);
    layout->addWidget(hint);

    auto* form = new QFormLayout();

    m_rate = new QComboBox(this);
    m_rate->setEditable(true);
    for (int r : {8000, 16000, 22050, 44100, 48000}) {
        m_rate->addItem(QString::number(r));
    }
    const QString rateStr = QString::number(initial.sampleRate);
    const int rateIdx = m_rate->findText(rateStr);
    if (rateIdx >= 0) {
        m_rate->setCurrentIndex(rateIdx);
    } else {
        m_rate->setEditText(rateStr);
    }
    form->addRow(tr("Частота, Гц:"), m_rate);

    m_channels = new QComboBox(this);
    m_channels->addItem(tr("1 (моно)"), 1);
    m_channels->addItem(tr("2 (стерео)"), 2);
    const int chIdx = m_channels->findData(initial.channels);
    m_channels->setCurrentIndex(chIdx >= 0 ? chIdx : 0);
    form->addRow(tr("Каналы:"), m_channels);

    m_format = new QComboBox(this);
    struct FmtItem { const char* title; AudioFileDecoder::RawFormat fmt; };
    static const FmtItem kItems[] = {
        { QT_TR_NOOP("16 бит знаковое LE (PCM s16le)"), AudioFileDecoder::RawFormat::S16LE },
        { QT_TR_NOOP("8 бит без знака (PCM u8)"),       AudioFileDecoder::RawFormat::S8U   },
        { QT_TR_NOOP("32 бит float LE (f32le)"),        AudioFileDecoder::RawFormat::F32LE },
        { QT_TR_NOOP("A-law (G.711) — телефония"),      AudioFileDecoder::RawFormat::ALAW  },
        { QT_TR_NOOP("μ-law (G.711) — телефония"),      AudioFileDecoder::RawFormat::ULAW  },
    };
    for (const FmtItem& it : kItems) {
        m_format->addItem(tr(it.title),
                          AudioFileDecoder::RawParams::formatToString(it.fmt));
    }
    const int fmtIdx = m_format->findData(
        AudioFileDecoder::RawParams::formatToString(initial.format));
    m_format->setCurrentIndex(fmtIdx >= 0 ? fmtIdx : 0);
    form->addRow(tr("Кодирование:"), m_format);

    layout->addLayout(form);

    auto* buttons = new QDialogButtonBox(
        QDialogButtonBox::Ok | QDialogButtonBox::Cancel, this);
    buttons->button(QDialogButtonBox::Ok)->setText(tr("Разобрать"));
    buttons->button(QDialogButtonBox::Cancel)->setText(tr("Отмена"));
    connect(buttons, &QDialogButtonBox::accepted, this, &QDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
    layout->addWidget(buttons);
}

AudioFileDecoder::RawParams RawParamsDialog::params() const
{
    AudioFileDecoder::RawParams p;

    bool ok = false;
    const int rate = m_rate->currentText().trimmed().toInt(&ok);
    p.sampleRate = (ok && rate >= 1000 && rate <= 384000) ? rate : 8000;

    p.channels = m_channels->currentData().toInt();
    if (p.channels < 1 || p.channels > 8) {
        p.channels = 1;
    }

    p.format = AudioFileDecoder::RawParams::formatFromString(
        m_format->currentData().toString());
    return p;
}
