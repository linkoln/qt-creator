/***************************************************************************
**
** This file is part of Qt Creator
**
** Copyright (c) 2008 Nokia Corporation and/or its subsidiary(-ies).
**
** Contact:  Qt Software Information (qt-info@nokia.com)
**
**
** Non-Open Source Usage
**
** Licensees may use this file in accordance with the Qt Beta Version
** License Agreement, Agreement version 2.2 provided with the Software or,
** alternatively, in accordance with the terms contained in a written
** agreement between you and Nokia.
**
** GNU General Public License Usage
**
** Alternatively, this file may be used under the terms of the GNU General
** Public License versions 2.0 or 3.0 as published by the Free Software
** Foundation and appearing in the file LICENSE.GPL included in the packaging
** of this file.  Please review the following information to ensure GNU
** General Public Licensing requirements will be met:
**
** http://www.fsf.org/licensing/licenses/info/GPLv2.html and
** http://www.gnu.org/copyleft/gpl.html.
**
** In addition, as a special exception, Nokia gives you certain additional
** rights. These rights are described in the Nokia Qt GPL Exception
** version 1.3, included in the file GPL_EXCEPTION.txt in this package.
**
***************************************************************************/

#include "fancyactionbar.h"

#include <QtGui/QHBoxLayout>
#include <QtGui/QPainter>
#include <QtGui/QPicture>
#include <QtGui/QVBoxLayout>
#include <QtSvg/QSvgRenderer>

using namespace Core;
using namespace Internal;

static const char* const svgIdButtonBase =               "ButtonBase";
static const char* const svgIdButtonNormalBase =         "ButtonNormalBase";
static const char* const svgIdButtonNormalOverlay =      "ButtonNormalOverlay";
static const char* const svgIdButtonPressedBase =        "ButtonPressedBase";
static const char* const svgIdButtonPressedOverlay =     "ButtonPressedOverlay";
static const char* const svgIdButtonDisabledOverlay =    "ButtonDisabledOverlay";
static const char* const svgIdButtonHoverOverlay =       "ButtonHoverOverlay";

static const char* const elementsSvgIds[] = {
    svgIdButtonBase,
    svgIdButtonNormalBase,
    svgIdButtonNormalOverlay,
    svgIdButtonPressedBase,
    svgIdButtonPressedOverlay,
    svgIdButtonDisabledOverlay,
    svgIdButtonHoverOverlay
};

const QMap<QString, QPicture> &buttonElementsMap()
{
    static QMap<QString, QPicture> result;
    if (result.isEmpty()) {
        QSvgRenderer renderer(QLatin1String(":/fancyactionbar/images/fancytoolbutton.svg"));
        for (size_t i = 0; i < sizeof(elementsSvgIds)/sizeof(elementsSvgIds[0]); i++) {
            QString elementId(elementsSvgIds[i]);
            QPicture elementPicture;
            QPainter elementPainter(&elementPicture);
            renderer.render(&elementPainter, elementId);
            result.insert(elementId, elementPicture);
        }
    }
    return result;
}

FancyToolButton::FancyToolButton(QWidget *parent)
    : QToolButton(parent)
    , m_buttonElements(buttonElementsMap())
{
    setAttribute(Qt::WA_Hover, true);
}

void FancyToolButton::paintEvent(QPaintEvent *event)
{
    Q_UNUSED(event)
    QPainter p(this);
    p.drawPicture(0, 0, m_buttonElements.value(svgIdButtonBase));
    p.drawPicture(0, 0, m_buttonElements.value(isDown() ? svgIdButtonPressedBase : svgIdButtonNormalBase));
#ifndef Q_WS_MAC // Mac UI's dont usually do hover
    if (underMouse() && isEnabled())
        p.drawPicture(0, 0, m_buttonElements.value(svgIdButtonHoverOverlay));
#endif
    if (!icon().isNull()) {
        icon().paint(&p, rect());
    } else {
        const int margin = 4;
        p.drawText(rect().adjusted(margin, margin, -margin, -margin), Qt::AlignCenter | Qt::TextWordWrap, text());
    }
    if (!isEnabled())
        p.drawPicture(0, 0, m_buttonElements.value(svgIdButtonDisabledOverlay));
    p.drawPicture(0, 0, m_buttonElements.value(isDown() ? svgIdButtonPressedOverlay : svgIdButtonNormalOverlay));
}

void FancyActionBar::paintEvent(QPaintEvent *event)
{
    Q_UNUSED(event)
}

QSize FancyToolButton::sizeHint() const
{
    return m_buttonElements.value(svgIdButtonBase).boundingRect().size();
}

FancyActionBar::FancyActionBar(QWidget *parent)
    : QWidget(parent)
{
    m_actionsLayout = new QVBoxLayout;

    QHBoxLayout *centeringLayout = new QHBoxLayout;
    centeringLayout->addStretch();
    centeringLayout->addLayout(m_actionsLayout);
    centeringLayout->addStretch();
    setLayout(centeringLayout);
}

void FancyActionBar::insertAction(int index, QAction *action, QMenu *menu)
{
    FancyToolButton *toolButton = new FancyToolButton(this);
    toolButton->setDefaultAction(action);
    if (menu) {
        toolButton->setMenu(menu);
        toolButton->setPopupMode(QToolButton::DelayedPopup);
    }
    m_actionsLayout->insertWidget(index, toolButton);
}
