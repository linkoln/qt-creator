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

#include "main.h"

#include <QtGui/QApplication>

MyMain::MyMain(QWidget *parent, Qt::WFlags flags)
    : QWidget(parent, flags)
{
    ui.setupUi(this);
    connect(ui.comboBox, SIGNAL(currentIndexChanged(int)), this, SLOT(select(int)));
}

void MyMain::add(IComboEntry *obj)
{
    m_entries.append(obj);
    ui.comboBox->addItem(obj->title());
}

void MyMain::select(int index)
{
    IComboEntry *entry = m_entries.at(index);
    // with multiple inheritance we would use qobject_cast here
    // instead we use query, to get the components if they exist
    IText1 *t1 = Aggregation::query<IText1>(entry);
    IText2 *t2 = Aggregation::query<IText2>(entry);
    IText3 *t3 = Aggregation::query<IText3>(entry);
    // set the label texts and enable/disable, depending on whether
    // the respective interface implementations exist
    ui.text1->setText(t1 ? t1->text() : tr("N/A"));
    ui.text2->setText(t2 ? t2->text() : tr("N/A"));
    ui.text3->setText(t3 ? t3->text() : tr("N/A"));
    ui.text1->setEnabled(t1);
    ui.text2->setEnabled(t2);
    ui.text3->setEnabled(t3);
}

MyMain::~MyMain()
{
    // the following deletes all the Aggregate and IComboEntry and ITextX
    // objects, as well as any other components we might have added
    qDeleteAll(m_entries);
}

int main(int argc, char *argv[])
{
    QApplication app(argc, argv);
    MyMain w;
    // create and set up some objects

    // the first does only implement the required IComboEntry
    // we don't need an aggregation for this
    IComboEntry *obj1 = new IComboEntry("Entry without text");

    // the second additionally provides an IText2 implementation
    // adding a component to the aggregation is done by setting the aggregation as the parent of the component
    Aggregation::Aggregate *obj2 = new Aggregation::Aggregate;
    obj2->add(new IComboEntry("Entry with text 2"));
    obj2->add(new IText2("This is a text for label 2"));

    // and so on... two more objects...
    Aggregation::Aggregate *obj3 = new Aggregation::Aggregate;
    obj3->add(new IComboEntry("Entry with text 1 and 2"));
    obj3->add(new IText1("I love Qt!"));
    obj3->add(new IText2("There are software companies..."));
    Aggregation::Aggregate *obj4 = new Aggregation::Aggregate;
    obj4->add(new IComboEntry("Entry with text 1 and 3"));
    obj4->add(new IText1("Some text written here."));
    obj4->add(new IText3("I'm a troll."));

    // the API takes IComboEntries, so we convert the them to it
    // the MyMain object takes the ownership of the whole aggregations
    w.add(Aggregation::query<IComboEntry>(obj1));
    w.add(Aggregation::query<IComboEntry>(obj2));
    w.add(Aggregation::query<IComboEntry>(obj3));
    w.add(Aggregation::query<IComboEntry>(obj4));
    w.show();
    return app.exec();
}
