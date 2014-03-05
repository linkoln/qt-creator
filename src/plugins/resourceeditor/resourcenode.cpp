/****************************************************************************
**
** Copyright (C) 2014 Digia Plc and/or its subsidiary(-ies).
** Contact: http://www.qt-project.org/legal
**
** This file is part of Qt Creator.
**
** Commercial License Usage
** Licensees holding valid commercial Qt licenses may use this file in
** accordance with the commercial license agreement provided with the
** Software or, alternatively, in accordance with the terms contained in
** a written agreement between you and Digia.  For licensing terms and
** conditions see http://qt.digia.com/licensing.  For further information
** use the contact form at http://qt.digia.com/contact-us.
**
** GNU Lesser General Public License Usage
** Alternatively, this file may be used under the terms of the GNU Lesser
** General Public License version 2.1 as published by the Free Software
** Foundation and appearing in the file LICENSE.LGPL included in the
** packaging of this file.  Please review the following information to
** ensure the GNU Lesser General Public License version 2.1 requirements
** will be met: http://www.gnu.org/licenses/old-licenses/lgpl-2.1.html.
**
** In addition, as a special exception, Digia gives you certain additional
** rights.  These rights are described in the Digia Qt LGPL Exception
** version 1.1, included in the file LGPL_EXCEPTION.txt in this package.
**
****************************************************************************/

#include "resourcenode.h"
#include "resourceeditorconstants.h"
#include "qrceditor/resourcefile_p.h"

#include <utils/fileutils.h>

#include <coreplugin/documentmanager.h>
#include <coreplugin/fileiconprovider.h>
#include <coreplugin/mimedatabase.h>

#include <qmljstools/qmljstoolsconstants.h>

#include <QDir>
#include <QDebug>

using namespace ResourceEditor;
using namespace ResourceEditor::Internal;

static int priority(const QStringList &files)
{
    if (files.isEmpty())
        return -1;
    Core::MimeType mt = Core::MimeDatabase::findByFile(files.at(0));
    QString type = mt.type();
    if (type.startsWith(QLatin1String("image/"))
            || type == QLatin1String(QmlJSTools::Constants::QML_MIMETYPE)
            || type == QLatin1String(QmlJSTools::Constants::JS_MIMETYPE))
        return 120;
    return 80;
}

static bool addFilesToResource(const QString &resourceFile, const QStringList &filePaths, QStringList *notAdded,
                     const QString &prefix, const QString &lang)
{
    if (notAdded)
        *notAdded = filePaths;

    ResourceFile file(resourceFile);
    if (!file.load())
        return false;

    int index = file.indexOfPrefix(prefix, lang);
    if (index == -1)
        index = file.addPrefix(prefix, lang);

    if (notAdded)
        notAdded->clear();
    foreach (const QString &path, filePaths) {
        if (file.contains(index, path))
            *notAdded << path;
        else
            file.addFile(index, path);
    }

    Core::DocumentManager::expectFileChange(resourceFile);
    file.save();
    Core::DocumentManager::unexpectFileChange(resourceFile);

    return true;
}

static bool sortByPrefixAndLang(ProjectExplorer::FolderNode *a, ProjectExplorer::FolderNode *b)
{
    ResourceFolderNode *aa = static_cast<ResourceFolderNode *>(a);
    ResourceFolderNode *bb = static_cast<ResourceFolderNode *>(b);

    if (aa->prefix() < bb->prefix())
        return true;
    if (bb->prefix() < aa->prefix())
        return false;
    return aa->lang() < bb->lang();
}

static bool sortNodesByPath(ProjectExplorer::Node *a, ProjectExplorer::Node *b)
{
    return a->path() < b->path();
}

ResourceTopLevelNode::ResourceTopLevelNode(const QString &filePath, FolderNode *parent)
    : ProjectExplorer::FolderNode(filePath)
{
    setIcon(Core::FileIconProvider::icon(filePath));
    m_document = new ResourceFileWatcher(this);
    Core::DocumentManager::addDocument(m_document);

    Utils::FileName base = Utils::FileName::fromString(parent->path());
    Utils::FileName file = Utils::FileName::fromString(filePath);
    if (file.isChildOf(base))
        setDisplayName(file.relativeChildPath(base).toString());
    else
        setDisplayName(file.toString());
}

ResourceTopLevelNode::~ResourceTopLevelNode()
{
    Core::DocumentManager::removeDocument(m_document);
}

void ResourceTopLevelNode::update()
{
    QList<ProjectExplorer::FolderNode *> newFolderList;
    QMap<QPair<QString, QString>, QList<ProjectExplorer::FileNode *> > filesToAdd;

    ResourceFile file(path());
    if (file.load()) {
        QSet<QPair<QString, QString > > prefixes;

        int prfxcount = file.prefixCount();
        for (int i = 0; i < prfxcount; ++i) {
            const QString &prefix = file.prefix(i);
            const QString &lang = file.lang(i);
            // ensure that we don't duplicate prefixes
            if (!prefixes.contains(qMakePair(prefix, lang))) {
                ProjectExplorer::FolderNode *fn = new ResourceFolderNode(file.prefix(i), file.lang(i), this);
                newFolderList << fn;

                prefixes.insert(qMakePair(prefix, lang));
            }

            QSet<QString> fileNames;
            int filecount = file.fileCount(i);
            for (int j = 0; j < filecount; ++j) {
                const QString &fileName = file.file(i, j);
                if (fileNames.contains(fileName)) {
                    // The file name is duplicated, skip it
                    // Note: this is wrong, but the qrceditor doesn't allow it either
                    // only aliases need to be unique
                } else {
                    fileNames.insert(fileName);
                    filesToAdd[qMakePair(prefix, lang)]
                            << new ResourceFileNode(fileName, this);
                }

            }
        }
    }

    QList<ProjectExplorer::FolderNode *> oldFolderList = subFolderNodes();
    QList<ProjectExplorer::FolderNode *> foldersToAdd;
    QList<ProjectExplorer::FolderNode *> foldersToRemove;

    std::sort(oldFolderList.begin(), oldFolderList.end(), sortByPrefixAndLang);
    std::sort(newFolderList.begin(), newFolderList.end(), sortByPrefixAndLang);

    ProjectExplorer::compareSortedLists(oldFolderList, newFolderList, foldersToRemove, foldersToAdd, sortByPrefixAndLang);

    removeFolderNodes(foldersToRemove);
    addFolderNodes(foldersToAdd);

    // delete nodes that weren't added
    qDeleteAll(ProjectExplorer::subtractSortedList(newFolderList, foldersToAdd, sortByPrefixAndLang));

    foreach (FolderNode *fn, subFolderNodes()) {
        ResourceFolderNode *rn = static_cast<ResourceFolderNode *>(fn);
        rn->updateFiles(filesToAdd.value(qMakePair(rn->prefix(), rn->lang())));
    }
}

QList<ProjectExplorer::ProjectAction> ResourceTopLevelNode::supportedActions(ProjectExplorer::Node *node) const
{
    if (node != this)
        return QList<ProjectExplorer::ProjectAction>();
    return QList<ProjectExplorer::ProjectAction>()
            << ProjectExplorer::AddNewFile
            << ProjectExplorer::AddExistingFile
            << ProjectExplorer::AddExistingDirectory
            << ProjectExplorer::HidePathActions
            << ProjectExplorer::Rename;
}

bool ResourceTopLevelNode::addFiles(const QStringList &filePaths, QStringList *notAdded)
{
    return addFilesToResource(path(), filePaths, notAdded, QLatin1String("/"), QString());
}

bool ResourceTopLevelNode::removeFiles(const QStringList &filePaths, QStringList *notRemoved)
{
    return parentFolderNode()->removeFiles(filePaths, notRemoved);
}

bool ResourceTopLevelNode::addPrefix(const QString &prefix, const QString &lang)
{
    ResourceFile file(path());
    if (!file.load())
        return false;
    int index = file.addPrefix(prefix, lang);
    if (index == -1)
        return false;
    Core::DocumentManager::expectFileChange(path());
    file.save();
    Core::DocumentManager::unexpectFileChange(path());

    update();

    return true;
}

bool ResourceTopLevelNode::removePrefix(const QString &prefix, const QString &lang)
{
    ResourceFile file(path());
    if (!file.load())
        return false;
    for (int i = 0; i < file.prefixCount(); ++i) {
        if (file.prefix(i) == prefix
                && file.lang(i) == lang) {
            file.removePrefix(i);
            Core::DocumentManager::expectFileChange(path());
            file.save();
            Core::DocumentManager::unexpectFileChange(path());

            update();
            return true;
        }
    }
    return false;
}

ProjectExplorer::FolderNode::AddNewInformation ResourceTopLevelNode::addNewInformation(const QStringList &files) const
{
    QString name = tr("%1 Prefix: %2")
            .arg(QFileInfo(path()).fileName())
            .arg(QLatin1String("/"));
    return AddNewInformation(name, priority(files) + 1);
}

ResourceFolderNode::ResourceFolderNode(const QString &prefix, const QString &lang, ResourceTopLevelNode *parent)
    : ProjectExplorer::FolderNode(parent->path() + QLatin1Char('/') + prefix),
      // TOOD Why add existing directory doesn't work
      m_topLevelNode(parent),
      m_prefix(prefix),
      m_lang(lang)
{

}

ResourceFolderNode::~ResourceFolderNode()
{

}

QList<ProjectExplorer::ProjectAction> ResourceFolderNode::supportedActions(ProjectExplorer::Node *node) const
{
    Q_UNUSED(node)
    QList<ProjectExplorer::ProjectAction> actions;
    actions << ProjectExplorer::AddNewFile
            << ProjectExplorer::AddExistingFile
            << ProjectExplorer::AddExistingDirectory
            << ProjectExplorer::RemoveFile
            << ProjectExplorer::Rename // Note: only works for the filename, works akwardly for relative file paths
            << ProjectExplorer::HidePathActions; // hides open terminal etc.

    // if the prefix is '/' (without lang) hide this node in add new dialog,
    // as the ResouceTopLevelNode is always shown for the '/' prefix
    if (m_prefix == QLatin1String("/") && m_lang.isEmpty())
        actions << ProjectExplorer::InheritedFromParent;

    return actions;
}

bool ResourceFolderNode::addFiles(const QStringList &filePaths, QStringList *notAdded)
{
    return addFilesToResource(m_topLevelNode->path(), filePaths, notAdded, m_prefix, m_lang);
}

bool ResourceFolderNode::removeFiles(const QStringList &filePaths, QStringList *notRemoved)
{
    if (notRemoved)
        *notRemoved = filePaths;
    ResourceFile file(m_topLevelNode->path());
    if (!file.load())
        return false;
    int index = file.indexOfPrefix(m_prefix, m_lang);
    if (index == -1)
        return false;
    for (int j = 0; j < file.fileCount(index); ++j) {
        QString fileName = file.file(index, j);
        if (!filePaths.contains(fileName))
            continue;
        if (notRemoved)
            notRemoved->removeOne(fileName);
        file.removeFile(index, j);
        --j;
    }
    Core::DocumentManager::expectFileChange(m_topLevelNode->path());
    file.save();
    Core::DocumentManager::unexpectFileChange(m_topLevelNode->path());

    return true;
}

bool ResourceFolderNode::renameFile(const QString &filePath, const QString &newFilePath)
{
    ResourceFile file(m_topLevelNode->path());
    if (!file.load())
        return false;
    int index = file.indexOfPrefix(m_prefix, m_lang);
    if (index == -1)
        return false;

    for (int j = 0; j < file.fileCount(index); ++j) {
        if (file.file(index, j) == filePath) {
            file.replaceFile(index, j, newFilePath);
            Core::DocumentManager::expectFileChange(m_topLevelNode->path());
            file.save();
            Core::DocumentManager::unexpectFileChange(m_topLevelNode->path());
            return true;
        }
    }

    return false;
}

bool ResourceFolderNode::renamePrefix(const QString &prefix, const QString &lang)
{
    ResourceFile file(m_topLevelNode->path());
    if (!file.load())
        return false;
    int index = file.indexOfPrefix(prefix, lang);
    if (index == -1)
        return false;

    if (!file.replacePrefixAndLang(index, prefix, lang))
        return false;

    Core::DocumentManager::expectFileChange(m_topLevelNode->path());
    file.save();
    Core::DocumentManager::unexpectFileChange(m_topLevelNode->path());
    return true;
}

ProjectExplorer::FolderNode::AddNewInformation ResourceFolderNode::addNewInformation(const QStringList &files) const
{
    QString name = tr("%1 Prefix: %2")
            .arg(QFileInfo(m_topLevelNode->path()).fileName())
            .arg(displayName());
    return AddNewInformation(name, priority(files) + 1);
}

QString ResourceFolderNode::displayName() const
{
    if (m_lang.isEmpty())
        return m_prefix;
    return m_prefix + QLatin1String(" (") + m_lang + QLatin1Char(')');
}

QString ResourceFolderNode::prefix() const
{
    return m_prefix;
}

QString ResourceFolderNode::lang() const
{
    return m_lang;
}

ResourceTopLevelNode *ResourceFolderNode::resourceNode() const
{
    return m_topLevelNode;
}

void ResourceFolderNode::updateFiles(QList<ProjectExplorer::FileNode *> newList)
{
    QList<ProjectExplorer::FileNode *> oldList = fileNodes();
    QList<ProjectExplorer::FileNode *> filesToAdd;
    QList<ProjectExplorer::FileNode *> filesToRemove;

    std::sort(oldList.begin(), oldList.end(), sortNodesByPath);
    std::sort(newList.begin(), newList.end(), sortNodesByPath);

    ProjectExplorer::compareSortedLists(oldList, newList, filesToRemove, filesToAdd, sortNodesByPath);

    removeFileNodes(filesToRemove);
    addFileNodes(filesToAdd);

    qDeleteAll(ProjectExplorer::subtractSortedList(newList, filesToAdd, sortNodesByPath));
}

ResourceFileWatcher::ResourceFileWatcher(ResourceTopLevelNode *node)
    : IDocument(node), m_node(node)
{
    setId("ResourceNodeWatcher");
    setFilePath(node->path());
}

bool ResourceFileWatcher::save(QString *errorString, const QString &fileName, bool autoSave)
{
    Q_UNUSED(errorString);
    Q_UNUSED(fileName);
    Q_UNUSED(autoSave);
    return false;
}

QString ResourceFileWatcher::defaultPath() const
{
    return QString();
}

QString ResourceFileWatcher::suggestedFileName() const
{
    return QString();
}

QString ResourceFileWatcher::mimeType() const
{
    return QLatin1String(ResourceEditor::Constants::C_RESOURCE_MIMETYPE);
}

bool ResourceFileWatcher::isModified() const
{
    return false;
}

bool ResourceFileWatcher::isSaveAsAllowed() const
{
    return false;
}

Core::IDocument::ReloadBehavior ResourceFileWatcher::reloadBehavior(ChangeTrigger state, ChangeType type) const
{
    Q_UNUSED(state)
    Q_UNUSED(type)
    return BehaviorSilent;
}

bool ResourceFileWatcher::reload(QString *errorString, ReloadFlag flag, ChangeType type)
{
    Q_UNUSED(errorString)
    Q_UNUSED(flag)
    if (type == TypePermissions)
        return true;
    m_node->update();
    return true;
}

ResourceFileNode::ResourceFileNode(const QString &filePath, ResourceTopLevelNode *topLevel)
    : ProjectExplorer::FileNode(filePath, ProjectExplorer::UnknownFileType, false),
      m_topLevel(topLevel)

{
    QString baseDir = QFileInfo(topLevel->path()).absolutePath();
    m_displayName = QDir(baseDir).relativeFilePath(filePath);
}

QString ResourceFileNode::displayName() const
{
    return m_displayName;
}
