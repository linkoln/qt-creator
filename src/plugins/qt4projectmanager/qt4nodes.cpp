/**************************************************************************
**
** This file is part of Qt Creator
**
** Copyright (c) 2010 Nokia Corporation and/or its subsidiary(-ies).
**
** Contact: Nokia Corporation (qt-info@nokia.com)
**
** Commercial Usage
**
** Licensees holding valid Qt Commercial licenses may use this file in
** accordance with the Qt Commercial License Agreement provided with the
** Software or, alternatively, in accordance with the terms contained in
** a written agreement between you and Nokia.
**
** GNU Lesser General Public License Usage
**
** Alternatively, this file may be used under the terms of the GNU Lesser
** General Public License version 2.1 as published by the Free Software
** Foundation and appearing in the file LICENSE.LGPL included in the
** packaging of this file.  Please review the following information to
** ensure the GNU Lesser General Public License version 2.1 requirements
** will be met: http://www.gnu.org/licenses/old-licenses/lgpl-2.1.html.
**
** If you are unsure which license is appropriate for your use, please
** contact the sales department at http://qt.nokia.com/contact.
**
**************************************************************************/

#include "profilereader.h"
#include "prowriter.h"
#include "qt4nodes.h"
#include "qt4project.h"
#include "qt4projectmanager.h"
#include "qt4projectmanagerconstants.h"
#include "qtuicodemodelsupport.h"
#include "qt4buildconfiguration.h"
#include "qmakestep.h"

#include <projectexplorer/nodesvisitor.h>

#include <coreplugin/editormanager/editormanager.h>
#include <coreplugin/editormanager/ieditor.h>
#include <coreplugin/fileiconprovider.h>
#include <coreplugin/filemanager.h>
#include <coreplugin/icore.h>
#include <coreplugin/iversioncontrol.h>
#include <coreplugin/vcsmanager.h>

#include <cpptools/cppmodelmanagerinterface.h>
#include <cplusplus/CppDocument.h>
#include <extensionsystem/pluginmanager.h>
#include <projectexplorer/projectexplorer.h>
#include <projectexplorer/buildmanager.h>

#include <utils/qtcassert.h>
#include <utils/stringutils.h>
#include <algorithm>

#include <QtCore/QDebug>
#include <QtCore/QDir>
#include <QtCore/QFile>
#include <QtCore/QFileInfo>
#include <QtCore/QCoreApplication>
#include <QtCore/QXmlStreamReader>

#include <QtGui/QPainter>
#include <QtGui/QMainWindow>
#include <QtGui/QMessageBox>
#include <QtGui/QPushButton>
#include <qtconcurrent/QtConcurrentTools>

// Static cached data in struct Qt4NodeStaticData providing information and icons
// for file types and the project. Do some magic via qAddPostRoutine()
// to make sure the icons do not outlive QApplication, triggering warnings on X11.

struct FileTypeDataStorage {
    ProjectExplorer::FileType type;
    const char *typeName;
    const char *icon;
};

static const FileTypeDataStorage fileTypeDataStorage[] = {
    { ProjectExplorer::HeaderType,
      QT_TRANSLATE_NOOP("Qt4ProjectManager::Internal::Qt4PriFileNode", "Headers"),
      ":/qt4projectmanager/images/headers.png" },
    { ProjectExplorer::SourceType,
      QT_TRANSLATE_NOOP("Qt4ProjectManager::Internal::Qt4PriFileNode", "Sources"),
      ":/qt4projectmanager/images/sources.png" },
    { ProjectExplorer::FormType,
      QT_TRANSLATE_NOOP("Qt4ProjectManager::Internal::Qt4PriFileNode", "Forms"),
      ":/qt4projectmanager/images/forms.png" },
    { ProjectExplorer::ResourceType,
      QT_TRANSLATE_NOOP("Qt4ProjectManager::Internal::Qt4PriFileNode", "Resources"),
      ":/qt4projectmanager/images/qt_qrc.png" },
    { ProjectExplorer::QMLType,
      QT_TRANSLATE_NOOP("Qt4ProjectManager::Internal::Qt4PriFileNode", "QML"),
      ":/qt4projectmanager/images/qml.ico" }, // TODO icon
    { ProjectExplorer::UnknownFileType,
      QT_TRANSLATE_NOOP("Qt4ProjectManager::Internal::Qt4PriFileNode", "Other files"),
      ":/qt4projectmanager/images/unknown.png" }
};

struct Qt4NodeStaticData {
    struct FileTypeData {
        FileTypeData(ProjectExplorer::FileType t = ProjectExplorer::UnknownFileType,
                     const QString &tN = QString(),
                     const QIcon &i = QIcon()) :
        type(t), typeName(tN), icon(i) { }

        ProjectExplorer::FileType type;
        QString typeName;
        QIcon icon;
    };

    QVector<FileTypeData> fileTypeData;
    QIcon projectIcon;
};

static void clearQt4NodeStaticData();

Q_GLOBAL_STATIC_WITH_INITIALIZER(Qt4NodeStaticData, qt4NodeStaticData, {
    // File type data
    const unsigned count = sizeof(fileTypeDataStorage)/sizeof(FileTypeDataStorage);
    x->fileTypeData.reserve(count);

    // Overlay the SP_DirIcon with the custom icons
    const QSize desiredSize = QSize(16, 16);

    for (unsigned i = 0 ; i < count; i++) {
        const QIcon overlayIcon = QIcon(QLatin1String(fileTypeDataStorage[i].icon));
        const QPixmap folderPixmap =
                Core::FileIconProvider::overlayIcon(QStyle::SP_DirIcon,
                                                    overlayIcon, desiredSize);
        QIcon folderIcon;
        folderIcon.addPixmap(folderPixmap);
        const QString desc = Qt4ProjectManager::Internal::Qt4PriFileNode::tr(fileTypeDataStorage[i].typeName);
        x->fileTypeData.push_back(Qt4NodeStaticData::FileTypeData(fileTypeDataStorage[i].type,
                                                                  desc, folderIcon));
    }
    // Project icon
    const QIcon projectBaseIcon(QLatin1String(":/qt4projectmanager/images/qt_project.png"));
    const QPixmap projectPixmap = Core::FileIconProvider::overlayIcon(QStyle::SP_DirIcon,
                                                                      projectBaseIcon,
                                                                      desiredSize);
    x->projectIcon.addPixmap(projectPixmap);

    qAddPostRoutine(clearQt4NodeStaticData);
})

static void clearQt4NodeStaticData()
{
    qt4NodeStaticData()->fileTypeData.clear();
    qt4NodeStaticData()->projectIcon = QIcon();
}

enum { debug = 0 };

namespace {
    // sorting helper function
    bool sortProjectFilesByPath(ProFile *f1, ProFile *f2)
    {
        return f1->fileName() < f2->fileName();
    }
}

namespace Qt4ProjectManager {
namespace Internal {

Qt4PriFile::Qt4PriFile(Qt4PriFileNode *qt4PriFile)
    : IFile(qt4PriFile), m_priFile(qt4PriFile)
{

}

bool Qt4PriFile::save(const QString &fileName)
{
    Q_UNUSED(fileName);
    return false;
}

void Qt4PriFile::rename(const QString &newName)
{
    // Can't happen
    Q_ASSERT(false);
    Q_UNUSED(newName);
}

QString Qt4PriFile::fileName() const
{
    return m_priFile->path();
}

QString Qt4PriFile::defaultPath() const
{
    return QString();
}

QString Qt4PriFile::suggestedFileName() const
{
    return QString();
}

QString Qt4PriFile::mimeType() const
{
    return Qt4ProjectManager::Constants::PROFILE_MIMETYPE;
}

bool Qt4PriFile::isModified() const
{
    return false;
}

bool Qt4PriFile::isReadOnly() const
{
    return false;
}

bool Qt4PriFile::isSaveAsAllowed() const
{
    return false;
}

Core::IFile::ReloadBehavior Qt4PriFile::reloadBehavior(ChangeTrigger state, ChangeType type) const
{
    Q_UNUSED(state)
    Q_UNUSED(type)
    return BehaviorSilent;
}

void Qt4PriFile::reload(ReloadFlag flag, ChangeType type)
{
    Q_UNUSED(flag)
    Q_UNUSED(type)
    if (type == TypePermissions)
        return;
    m_priFile->scheduleUpdate();
}

/*!
  \class Qt4PriFileNode
  Implements abstract ProjectNode class
  */

Qt4PriFileNode::Qt4PriFileNode(Qt4Project *project, Qt4ProFileNode* qt4ProFileNode, const QString &filePath)
        : ProjectNode(filePath),
          m_project(project),
          m_qt4ProFileNode(qt4ProFileNode),
          m_projectFilePath(QDir::fromNativeSeparators(filePath)),
          m_projectDir(QFileInfo(filePath).absolutePath())
{
    Q_ASSERT(project);
    m_qt4PriFile = new Qt4PriFile(this);
    Core::ICore::instance()->fileManager()->addFile(m_qt4PriFile);

    setDisplayName(QFileInfo(filePath).completeBaseName());

    setIcon(qt4NodeStaticData()->projectIcon);
}

void Qt4PriFileNode::scheduleUpdate()
{
    ProFileCacheManager::instance()->discardFile(m_projectFilePath);
    m_qt4ProFileNode->scheduleUpdate();
}

struct InternalNode
{
    QMap<QString, InternalNode*> subnodes;
    QStringList files;
    ProjectExplorer::FileType type;
    QString displayName;
    QString fullPath;
    QIcon icon;

    InternalNode()
    {
        type = ProjectExplorer::UnknownFileType;
    }

    ~InternalNode()
    {
        qDeleteAll(subnodes);
    }

    // Creates a tree structure from a list of absolute file paths.
    // Empty directories are compressed into a single entry with a longer path.
    // * project
    //    * /absolute/path
    //       * file1
    //    * relative
    //       * path1
    //          * file1
    //          * file2
    //       * path2
    //          * file1
    // The method first creates a tree that looks like the directory structure, i.e.
    //    * /
    //       * absolute
    //          * path
    // ...
    // and afterwards calls compress() which merges directory nodes with single children, i.e. to
    //    * /absolute/path
    void create(const QString &projectDir, const QStringList &newFilePaths, ProjectExplorer::FileType type)
    {
        static const QChar separator = QChar('/');
        const QString projectDirWithSeparator = projectDir + separator;
        int projectDirWithSeparatorLength = projectDirWithSeparator.length();
        foreach (const QString &file, newFilePaths) {
            QString fileWithoutPrefix;
            bool isRelative;
            if (file.startsWith(projectDirWithSeparator)) {
                isRelative = true;
                fileWithoutPrefix = file.mid(projectDirWithSeparatorLength);
            } else {
                isRelative = false;
                fileWithoutPrefix = file;
            }
            QStringList parts = fileWithoutPrefix.split(separator, QString::SkipEmptyParts);
#ifndef Q_OS_WIN
            if (!isRelative && parts.count() > 0)
                parts[0].prepend(separator);
#endif
            QStringListIterator it(parts);
            InternalNode *currentNode = this;
            QString path = (isRelative ? projectDirWithSeparator : "");
            while (it.hasNext()) {
                const QString &key = it.next();
                if (it.hasNext()) { // key is directory
                    path += key;
                    if (!currentNode->subnodes.contains(path)) {
                        InternalNode *val = new InternalNode;
                        val->type = type;
                        val->fullPath = path;
                        val->displayName = key;
                        currentNode->subnodes.insert(path, val);
                        currentNode = val;
                    } else {
                        currentNode = currentNode->subnodes.value(path);
                    }
                    path += separator;
                } else { // key is filename
                    currentNode->files.append(file);
                }
            }
        }
        this->compress();
    }

    // Removes folder nodes with only a single sub folder in it
    void compress()
    {
        QMap<QString, InternalNode*> newSubnodes;
        QMapIterator<QString, InternalNode*> i(subnodes);
        while (i.hasNext()) {
            i.next();
            i.value()->compress();
            if (i.value()->files.isEmpty() && i.value()->subnodes.size() == 1) {
                // replace i.value() by i.value()->subnodes.begin()
                QString key = i.value()->subnodes.begin().key();
                InternalNode *keep = i.value()->subnodes.value(key);
                keep->displayName = i.value()->displayName + "/" + keep->displayName;
                newSubnodes.insert(key, keep);
                i.value()->subnodes.clear();
                delete i.value();
            } else {
                newSubnodes.insert(i.key(), i.value());
            }
        }
        subnodes = newSubnodes;
    }

    // Makes the projectNode's subtree below the given folder match this internal node's subtree
    void updateSubFolders(Qt4PriFileNode *projectNode, ProjectExplorer::FolderNode *folder)
    {
        updateFiles(projectNode, folder, type);

        // update folders
        QList<FolderNode *> existingFolderNodes;
        foreach (FolderNode *node, folder->subFolderNodes()) {
            if (node->nodeType() != ProjectNodeType)
                existingFolderNodes << node;
        }

        qSort(existingFolderNodes.begin(), existingFolderNodes.end(), ProjectNode::sortNodesByPath);

        QList<FolderNode *> foldersToRemove;
        QList<FolderNode *> foldersToAdd;
        typedef QPair<InternalNode *, FolderNode *> NodePair;
        QList<NodePair> nodesToUpdate;

        // Both lists should be already sorted...
        QList<FolderNode*>::const_iterator existingNodeIter = existingFolderNodes.constBegin();
        QMap<QString, InternalNode*>::const_iterator newNodeIter = subnodes.constBegin();;
        while (existingNodeIter != existingFolderNodes.constEnd()
               && newNodeIter != subnodes.constEnd()) {
            if ((*existingNodeIter)->path() < newNodeIter.value()->fullPath) {
                foldersToRemove << *existingNodeIter;
                ++existingNodeIter;
            } else if ((*existingNodeIter)->path() > newNodeIter.value()->fullPath) {
                FolderNode *newNode = new FolderNode(newNodeIter.value()->fullPath);
                newNode->setDisplayName(newNodeIter.value()->displayName);
                if (!newNodeIter.value()->icon.isNull())
                    newNode->setIcon(newNodeIter.value()->icon);
                foldersToAdd << newNode;
                nodesToUpdate << NodePair(newNodeIter.value(), newNode);
                ++newNodeIter;
            } else { // *existingNodeIter->path() == *newPathIter
                nodesToUpdate << NodePair(newNodeIter.value(), *existingNodeIter);
                ++existingNodeIter;
                ++newNodeIter;
            }
        }

        while (existingNodeIter != existingFolderNodes.constEnd()) {
            foldersToRemove << *existingNodeIter;
            ++existingNodeIter;
        }
        while (newNodeIter != subnodes.constEnd()) {
            FolderNode *newNode = new FolderNode(newNodeIter.value()->fullPath);
            newNode->setDisplayName(newNodeIter.value()->displayName);
            if (!newNodeIter.value()->icon.isNull())
                newNode->setIcon(newNodeIter.value()->icon);
            foldersToAdd << newNode;
            nodesToUpdate << NodePair(newNodeIter.value(), newNode);
            ++newNodeIter;
        }

        if (!foldersToRemove.isEmpty())
            projectNode->removeFolderNodes(foldersToRemove, folder);
        if (!foldersToAdd.isEmpty())
            projectNode->addFolderNodes(foldersToAdd, folder);

        foreach (const NodePair &np, nodesToUpdate)
            np.first->updateSubFolders(projectNode, np.second);
    }

    // Makes the folder's files match this internal node's file list
    void updateFiles(Qt4PriFileNode *projectNode, FolderNode *folder, FileType type)
    {
        QList<FileNode*> existingFileNodes;
        foreach (FileNode *fileNode, folder->fileNodes()) {
            if (fileNode->fileType() == type && !fileNode->isGenerated())
                existingFileNodes << fileNode;
        }

        QList<FileNode*> filesToRemove;
        QList<FileNode*> filesToAdd;

        qSort(files);
        qSort(existingFileNodes.begin(), existingFileNodes.end(), ProjectNode::sortNodesByPath);

        QList<FileNode*>::const_iterator existingNodeIter = existingFileNodes.constBegin();
        QList<QString>::const_iterator newPathIter = files.constBegin();
        while (existingNodeIter != existingFileNodes.constEnd()
               && newPathIter != files.constEnd()) {
            if ((*existingNodeIter)->path() < *newPathIter) {
                filesToRemove << *existingNodeIter;
                ++existingNodeIter;
            } else if ((*existingNodeIter)->path() > *newPathIter) {
                filesToAdd << new FileNode(*newPathIter, type, false);
                ++newPathIter;
            } else { // *existingNodeIter->path() == *newPathIter
                ++existingNodeIter;
                ++newPathIter;
            }
        }
        while (existingNodeIter != existingFileNodes.constEnd()) {
            filesToRemove << *existingNodeIter;
            ++existingNodeIter;
        }
        while (newPathIter != files.constEnd()) {
            filesToAdd << new FileNode(*newPathIter, type, false);
            ++newPathIter;
        }

        if (!filesToRemove.isEmpty())
            projectNode->removeFileNodes(filesToRemove, folder);
        if (!filesToAdd.isEmpty())
            projectNode->addFileNodes(filesToAdd, folder);
    }
};


QStringList Qt4PriFileNode::baseVPaths(ProFileReader *reader, const QString &projectDir)
{
    QStringList result;
    if (!reader)
        return result;
    result += reader->absolutePathValues("VPATH", projectDir);
    result << projectDir; // QMAKE_ABSOLUTE_SOURCE_PATH
    result += reader->absolutePathValues("DEPENDPATH", projectDir);
    result.removeDuplicates();
    return result;
}

QStringList Qt4PriFileNode::fullVPaths(const QStringList &baseVPaths, ProFileReader *reader, FileType type, const QString &qmakeVariable, const QString &projectDir)
{
    QStringList vPaths;
    if (!reader)
        return vPaths;
    if (type == ProjectExplorer::SourceType)
        vPaths = reader->absolutePathValues("VPATH_" + qmakeVariable, projectDir);
    vPaths += baseVPaths;
    if (type == ProjectExplorer::HeaderType)
        vPaths += reader->absolutePathValues("INCLUDEPATH", projectDir);
    vPaths.removeDuplicates();
    return vPaths;
}

static QSet<QString> recursiveEnumerate(const QString &folder)
{
    QSet<QString> result;
    QFileInfo fi(folder);
    if (fi.isDir()) {
        QDir dir(folder);
        dir.setFilter(dir.filter() | QDir::NoDotAndDotDot);

        foreach (const QFileInfo &file, dir.entryInfoList()) {
            if (file.isDir())
                result += recursiveEnumerate(file.absoluteFilePath());
            else
                result += file.absoluteFilePath();
        }
    } else if (fi.exists()) {
        result << folder;
    }
    return result;
}

void Qt4PriFileNode::update(ProFile *includeFileExact, ProFileReader *readerExact, ProFile *includeFileCumlative, ProFileReader *readerCumulative)
{
    // add project file node
    if (m_fileNodes.isEmpty())
        addFileNodes(QList<FileNode*>() << new FileNode(m_projectFilePath, ProjectFileType, false), this);

    const QString &projectDir = m_qt4ProFileNode->m_projectDir;

    QStringList baseVPathsExact = baseVPaths(readerExact, projectDir);
    QStringList baseVPathsCumulative = baseVPaths(readerCumulative, projectDir);

    const QVector<Qt4NodeStaticData::FileTypeData> &fileTypes = qt4NodeStaticData()->fileTypeData;

    InternalNode contents;

    // Figure out DEPLOYMENT and INSTALL folders
    QStringList folders;
    QStringList dynamicVariables = dynamicVarNames(readerExact, readerCumulative);
    foreach (const QString &dynamicVar, dynamicVariables) {
        folders += readerExact->values(dynamicVar, includeFileExact);
        // Ignore stuff from cumulative parse
        // we are recursively enumerating all the files from those folders
        // and add watchers for them, that's too dangerous if we get the foldrs
        // wrong and enumerate the whole project tree multiple times
    }


    for (int i=0; i < folders.size(); ++i) {
        QFileInfo fi(folders.at(i));
        if (fi.isRelative())
            folders[i] = projectDir + "/" + folders.at(i);
    }


    m_recursiveEnumerateFiles.clear();
    // Remove non existing items and non folders
    // todo fix files in INSTALL rules
    QStringList::iterator it = folders.begin();
    while (it != folders.end()) {
        QFileInfo fi(*it);
        if (fi.exists()) {
            if (fi.isDir()) {
                // keep directories
                ++it;
            } else {
                // move files directly to m_recursiveEnumerateFiles
                m_recursiveEnumerateFiles << *it;
                it = folders.erase(it);
            }
        } else {
            // do remove non exsting stuff
            it = folders.erase(it);
        }
    }

    folders.removeDuplicates();
    watchFolders(folders.toSet());

    foreach (const QString &folder, folders) {
        m_recursiveEnumerateFiles += recursiveEnumerate(folder);
    }


    QMap<FileType, QSet<QString> > foundFiles;

    // update files
    for (int i = 0; i < fileTypes.size(); ++i) {
        FileType type = fileTypes.at(i).type;
        QStringList qmakeVariables = varNames(type);

        QSet<QString> newFilePaths;
        foreach (const QString &qmakeVariable, qmakeVariables) {
            QStringList vPathsExact = fullVPaths(baseVPathsExact, readerExact, type, qmakeVariable, projectDir);
            QStringList vPathsCumulative = fullVPaths(baseVPathsCumulative, readerCumulative, type, qmakeVariable, projectDir);

            newFilePaths += readerExact->absoluteFileValues(qmakeVariable, projectDir, vPathsExact, includeFileExact).toSet();
            if (readerCumulative)
                newFilePaths += readerCumulative->absoluteFileValues(qmakeVariable, projectDir, vPathsCumulative, includeFileCumlative).toSet();

        }

        foundFiles[type] = newFilePaths;
        m_recursiveEnumerateFiles.subtract(newFilePaths);
    }

    for (int i = 0; i < fileTypes.size(); ++i) {
        FileType type = fileTypes.at(i).type;
        QSet<QString> newFilePaths = foundFiles[type];
        newFilePaths += filterFiles(type, m_recursiveEnumerateFiles);

        // We only need to save this information if
        // we are watching folders
        if (!folders.isEmpty())
            m_files[type] = newFilePaths;
        else
            m_files[type].clear();

        if (!newFilePaths.isEmpty()) {
            InternalNode *subfolder = new InternalNode;
            subfolder->type = type;
            subfolder->icon = fileTypes.at(i).icon;
            subfolder->fullPath = m_projectDir + "/#" + QString::number(i) + fileTypes.at(i).typeName;
            subfolder->displayName = fileTypes.at(i).typeName;
            contents.subnodes.insert(subfolder->fullPath, subfolder);
            // create the hierarchy with subdirectories
            subfolder->create(m_projectDir, newFilePaths.toList(), type);
        }
    }

    contents.updateSubFolders(this, this);
}

void Qt4PriFileNode::watchFolders(const QSet<QString> &folders)
{
    QSet<QString> toUnwatch = m_watchedFolders;
    toUnwatch.subtract(folders);

    QSet<QString> toWatch = folders;
    toWatch.subtract(m_watchedFolders);

    if (!toUnwatch.isEmpty())
        m_project->centralizedFolderWatcher()->unwatchFolders(toUnwatch.toList(), this);
    if (!toWatch.isEmpty())
        m_project->centralizedFolderWatcher()->watchFolders(toWatch.toList(), this);

    m_watchedFolders = folders;
}

void Qt4PriFileNode::folderChanged(const QString &folder)
{
    //qDebug()<<"########## Qt4PriFileNode::folderChanged";
    // So, we need to figure out which files changed.

    QString changedFolder = folder;
    if (!changedFolder.endsWith(QLatin1Char('/')))
        changedFolder.append(QLatin1Char('/'));

    // Collect all the files
    QSet<QString> newFiles;
    newFiles += recursiveEnumerate(changedFolder);

    foreach (const QString &file, m_recursiveEnumerateFiles) {
        if (!file.startsWith(changedFolder))
            newFiles.insert(file);
    }

    QSet<QString> addedFiles = newFiles;
    addedFiles.subtract(m_recursiveEnumerateFiles);

    QSet<QString> removedFiles = m_recursiveEnumerateFiles;
    removedFiles.subtract(newFiles);

    if (addedFiles.isEmpty() && removedFiles.isEmpty())
        return;

    m_recursiveEnumerateFiles = newFiles;

    // Apply the differences
    // per file type
    const QVector<Qt4NodeStaticData::FileTypeData> &fileTypes = qt4NodeStaticData()->fileTypeData;
    for (int i = 0; i < fileTypes.size(); ++i) {
        FileType type = fileTypes.at(i).type;
        QSet<QString> add = filterFiles(type, addedFiles);
        QSet<QString> remove = filterFiles(type, removedFiles);

        if (!add.isEmpty() || !remove.isEmpty()) {
            // Scream :)
//            qDebug()<<"For type"<<fileTypes.at(i).typeName<<"\n"
//                    <<"added files"<<add<<"\n"
//                    <<"removed files"<<remove;

            m_files[type].unite(add);
            m_files[type].subtract(remove);
        }
    }

    // Now apply stuff
    InternalNode contents;
    for (int i = 0; i < fileTypes.size(); ++i) {
        FileType type = fileTypes.at(i).type;
        if (!m_files[type].isEmpty()) {
            InternalNode *subfolder = new InternalNode;
            subfolder->type = type;
            subfolder->icon = fileTypes.at(i).icon;
            subfolder->fullPath = m_projectDir + "/#" + QString::number(i) + fileTypes.at(i).typeName;
            subfolder->displayName = fileTypes.at(i).typeName;
            contents.subnodes.insert(subfolder->fullPath, subfolder);
            // create the hierarchy with subdirectories
            subfolder->create(m_projectDir, m_files[type].toList(), type);
        }
    }

    contents.updateSubFolders(this, this);
    m_project->updateFileList();

    // The files to be packaged are listed inside the symbian build system.
    // We need to regenerate that list by running qmake
    // Other platforms do not have a explicit list of files to package, but package
    // directories
    foreach (ProjectExplorer::Target *target, m_project->targets()) {
        if (target->id() == Constants::S60_DEVICE_TARGET_ID) {
            foreach (ProjectExplorer::BuildConfiguration *bc, target->buildConfigurations()) {
                Qt4BuildConfiguration *qt4bc = qobject_cast<Qt4BuildConfiguration *>(bc);
                if (qt4bc) {
                    QMakeStep *qmakeStep = qt4bc->qmakeStep();
                    if (qmakeStep)
                        qmakeStep->setForced(true);
                }
            }
        }
    }

}

bool Qt4PriFileNode::deploysFolder(const QString &folder) const
{
    QString f = folder;
    if (!f.endsWith('/'))
        f.append('/');
    foreach (const QString &wf, m_watchedFolders) {
        if (f.startsWith(wf)
            && (wf.endsWith('/')
                || (wf.length() < f.length() && f.at(wf.length()) == '/')))
            return true;
    }
    return false;
}

QList<ProjectNode::ProjectAction> Qt4PriFileNode::supportedActions(Node *node) const
{
    QList<ProjectAction> actions;

    const FolderNode *folderNode = this;
    const Qt4ProFileNode *proFileNode;
    while (!(proFileNode = qobject_cast<const Qt4ProFileNode*>(folderNode)))
        folderNode = folderNode->parentFolderNode();
    Q_ASSERT(proFileNode);

    switch (proFileNode->projectType()) {
    case ApplicationTemplate:
    case LibraryTemplate: {
        actions << AddNewFile;
        if (m_recursiveEnumerateFiles.contains(node->path())) {
            actions << EraseFile;
        } else {
            actions << RemoveFile;
        }

        bool addExistingFiles = true;
        if (node->path().contains('#')) {
            // A virtual folder, we do what the projectexplorer does
            FolderNode *folder = qobject_cast<FolderNode *>(node);
            if (folder) {
                QStringList list;
                foreach (FolderNode *f, folder->subFolderNodes())
                    list << f->path() + '/';
                if (deploysFolder(Utils::commonPath(list)))
                    addExistingFiles = false;
            }
        }

        addExistingFiles = addExistingFiles && !deploysFolder(node->path());

        if (addExistingFiles)
            actions << AddExistingFile;

        break;
    }
    case SubDirsTemplate:
        actions << AddSubProject << RemoveSubProject;
        break;
    default:
        break;
    }

    FileNode *fileNode = qobject_cast<FileNode *>(node);
    if (fileNode && fileNode->fileType() != ProjectExplorer::ProjectFileType)
        actions << Rename;

    return actions;
}

bool Qt4PriFileNode::canAddSubProject(const QString &proFilePath) const
{
    QFileInfo fi(proFilePath);
    if (fi.suffix() == QLatin1String("pro")
        || fi.suffix() == QLatin1String("pri"))
        return true;
    return false;
}

static QString simplifyProFilePath(const QString &proFilePath)
{
    // if proFilePath is like: _path_/projectName/projectName.pro
    // we simplify it to: _path_/projectName
    QFileInfo fi(proFilePath);
    const QString parentPath = fi.absolutePath();
    QFileInfo parentFi(parentPath);
    if (parentFi.fileName() == fi.completeBaseName())
        return parentPath;
    return proFilePath;
}

bool Qt4PriFileNode::addSubProjects(const QStringList &proFilePaths)
{
    ProjectExplorer::FindAllFilesVisitor visitor;
    accept(&visitor);
    const QStringList &allFiles = visitor.filePaths();

    QStringList uniqueProFilePaths;
    foreach (const QString &proFile, proFilePaths)
        if (!allFiles.contains(proFile))
            uniqueProFilePaths.append(simplifyProFilePath(proFile));

    QStringList failedFiles;
    changeFiles(ProjectExplorer::ProjectFileType, uniqueProFilePaths, &failedFiles, AddToProFile);

    return failedFiles.isEmpty();
}

bool Qt4PriFileNode::removeSubProjects(const QStringList &proFilePaths)
{
    QStringList failedOriginalFiles;
    changeFiles(ProjectExplorer::ProjectFileType, proFilePaths, &failedOriginalFiles, RemoveFromProFile);

    QStringList simplifiedProFiles;
    foreach (const QString &proFile, failedOriginalFiles)
        simplifiedProFiles.append(simplifyProFilePath(proFile));

    QStringList failedSimplifiedFiles;
    changeFiles(ProjectExplorer::ProjectFileType, simplifiedProFiles, &failedSimplifiedFiles, RemoveFromProFile);

    return failedSimplifiedFiles.isEmpty();
}

bool Qt4PriFileNode::addFiles(const FileType fileType, const QStringList &filePaths,
                           QStringList *notAdded)
{
    // If a file is already referenced in the .pro file then we don't add them.
    // That ignores scopes and which variable was used to reference the file
    // So it's obviously a bit limited, but in those cases you need to edit the
    // project files manually anyway.

    ProjectExplorer::FindAllFilesVisitor visitor;
    accept(&visitor);
    const QStringList &allFiles = visitor.filePaths();

    QStringList qrcFiles; // the list of qrc files referenced from ui files
    if (fileType == ProjectExplorer::FormType) {
        foreach (const QString &formFile, filePaths) {
            QStringList resourceFiles = formResources(formFile);
            foreach (const QString &resourceFile, resourceFiles)
                if (!qrcFiles.contains(resourceFile))
                    qrcFiles.append(resourceFile);
        }
    }

    QStringList uniqueQrcFiles;
    foreach (const QString &file, qrcFiles) {
        if (!allFiles.contains(file))
            uniqueQrcFiles.append(file);
    }

    QStringList uniqueFilePaths;
    foreach (const QString &file, filePaths) {
        if (!allFiles.contains(file))
            uniqueFilePaths.append(file);
    }

    QStringList failedFiles;
    changeFiles(fileType, uniqueFilePaths, &failedFiles, AddToProFile);
    if (notAdded)
        *notAdded = failedFiles;
    changeFiles(ProjectExplorer::ResourceType, uniqueQrcFiles, &failedFiles, AddToProFile);
    if (notAdded)
        *notAdded += failedFiles;
    return failedFiles.isEmpty();
}

bool Qt4PriFileNode::removeFiles(const FileType fileType, const QStringList &filePaths,
                              QStringList *notRemoved)
{
    QStringList failedFiles;
    changeFiles(fileType, filePaths, &failedFiles, RemoveFromProFile);
    if (notRemoved)
        *notRemoved = failedFiles;
    return failedFiles.isEmpty();
}

bool Qt4PriFileNode::deleteFiles(const FileType fileType, const QStringList &filePaths)
{
    QStringList failedFiles;
    changeFiles(fileType, filePaths, &failedFiles, RemoveFromProFile);
    return true;
}

bool Qt4PriFileNode::renameFile(const FileType fileType, const QString &filePath,
                             const QString &newFilePath)
{
    if (newFilePath.isEmpty())
        return false;

    QStringList dummy;
    changeFiles(fileType, QStringList() << filePath, &dummy, RemoveFromProFile);
    if (!dummy.isEmpty())
        return false;
    changeFiles(fileType, QStringList() << newFilePath, &dummy, AddToProFile);
    if (!dummy.isEmpty())
        return false;
    return true;
}

bool Qt4PriFileNode::changeIncludes(ProFile *includeFile, const QStringList &proFilePaths,
                                    ChangeType change)
{
    Q_UNUSED(includeFile)
    Q_UNUSED(proFilePaths)
    Q_UNUSED(change)
    // TODO
    return false;
}

bool Qt4PriFileNode::priFileWritable(const QString &path)
{
    const QString dir = QFileInfo(path).dir().path();
    Core::ICore *core = Core::ICore::instance();
    Core::IVersionControl *versionControl = core->vcsManager()->findVersionControlForDirectory(dir);
    switch (Core::FileManager::promptReadOnlyFile(path, versionControl, core->mainWindow(), false)) {
    case Core::FileManager::RO_OpenVCS:
        if (!versionControl->vcsOpen(path)) {
            QMessageBox::warning(core->mainWindow(), tr("Failed!"), tr("Could not open the file for edit with VCS."));
            return false;
        }
        break;
    case Core::FileManager::RO_MakeWriteable: {
        const bool permsOk = QFile::setPermissions(path, QFile::permissions(path) | QFile::WriteUser);
        if (!permsOk) {
            QMessageBox::warning(core->mainWindow(), tr("Failed!"),  tr("Could not set permissions to writable."));
            return false;
        }
        break;
    }
    case Core::FileManager::RO_SaveAs:
    case Core::FileManager::RO_Cancel:
        return false;
    }
    return true;
}

bool Qt4PriFileNode::saveModifiedEditors()
{
    QList<Core::IFile*> modifiedFileHandles;

    Core::ICore *core = Core::ICore::instance();

    foreach (Core::IEditor *editor, core->editorManager()->editorsForFileName(m_projectFilePath)) {
        if (Core::IFile *editorFile = editor->file()) {
            if (editorFile->isModified())
                modifiedFileHandles << editorFile;
        }
    }

    if (!modifiedFileHandles.isEmpty()) {
        bool cancelled;
        core->fileManager()->saveModifiedFiles(modifiedFileHandles, &cancelled,
                                         tr("There are unsaved changes for project file %1.").arg(m_projectFilePath));
        if (cancelled)
            return false;
        // force instant reload of ourselves
        ProFileCacheManager::instance()->discardFile(m_projectFilePath);
        m_project->qt4ProjectManager()->notifyChanged(m_projectFilePath);
    }
    return true;
}

QStringList Qt4PriFileNode::formResources(const QString &formFile) const
{
    QStringList resourceFiles;
    QFile file(formFile);
    file.open(QIODevice::ReadOnly);
    QXmlStreamReader reader(&file);

    QFileInfo fi(formFile);
    QDir formDir = fi.absoluteDir();
    while (!reader.atEnd()) {
        reader.readNext();
        if (reader.isStartElement()) {
            if (reader.name() == QLatin1String("iconset")) {
                const QXmlStreamAttributes attributes = reader.attributes();
                if (attributes.hasAttribute(QLatin1String("resource")))
                    resourceFiles.append(QDir::cleanPath(formDir.absoluteFilePath(
                                  attributes.value(QLatin1String("resource")).toString())));
            } else if (reader.name() == QLatin1String("include")) {
                const QXmlStreamAttributes attributes = reader.attributes();
                if (attributes.hasAttribute(QLatin1String("location")))
                    resourceFiles.append(QDir::cleanPath(formDir.absoluteFilePath(
                                  attributes.value(QLatin1String("location")).toString())));

            }
        }
    }

    if (reader.hasError())
        qWarning() << "Could not read form file:" << formFile;

    return resourceFiles;
}

void Qt4PriFileNode::changeFiles(const FileType fileType,
                                 const QStringList &filePaths,
                                 QStringList *notChanged,
                                 ChangeType change)
{
    if (filePaths.isEmpty())
        return;

    *notChanged = filePaths;

    // Check for modified editors
    if (!saveModifiedEditors())
        return;

    // Ensure that the file is not read only
    QFileInfo fi(m_projectFilePath);
    if (!fi.isWritable()) {
        // Try via vcs manager
        Core::VcsManager *vcsManager = Core::ICore::instance()->vcsManager();
        Core::IVersionControl *versionControl = vcsManager->findVersionControlForDirectory(fi.absolutePath());
        if (!versionControl || versionControl->vcsOpen(m_projectFilePath)) {
            bool makeWritable = QFile::setPermissions(m_projectFilePath, fi.permissions() | QFile::WriteUser);
            if (!makeWritable) {
                QMessageBox::warning(Core::ICore::instance()->mainWindow(),
                                     tr("Failed!"),
                                     tr("Could not write project file %1.").arg(m_projectFilePath));
                return;
            }
        }
    }

    QStringList lines;
    ProFile *includeFile;
    {
        QString contents;
        {
            QFile qfile(m_projectFilePath);
            if (qfile.open(QIODevice::ReadOnly | QIODevice::Text)) {
                contents = QString::fromLocal8Bit(qfile.readAll());
                qfile.close();
                lines = contents.split(QLatin1Char('\n'));
                while (!lines.isEmpty() && lines.last().isEmpty())
                    lines.removeLast();
            } else {
                m_project->proFileParseError(tr("Error while reading .pro file %1: %2")
                                             .arg(m_projectFilePath, qfile.errorString()));
                return;
            }
        }

        ProMessageHandler handler;
        ProFileParser parser(0, &handler);
        includeFile = parser.parsedProBlock(m_projectFilePath, contents);
    }

    const QStringList vars = varNames(fileType);
    QDir priFileDir = QDir(m_qt4ProFileNode->m_projectDir);

    if (change == AddToProFile) {
        // Use the first variable for adding.
        // Yes, that's broken for adding objective c sources or other stuff.
        ProWriter::addFiles(includeFile, &lines, priFileDir, filePaths, vars.first());
        notChanged->clear();
    } else { // RemoveFromProFile
        *notChanged = ProWriter::removeFiles(includeFile, &lines, priFileDir, filePaths, vars);
    }

    // save file
    Core::ICore::instance()->fileManager()->expectFileChange(m_projectFilePath);
    save(lines);
    Core::ICore::instance()->fileManager()->unexpectFileChange(m_projectFilePath);

    // This is a hack.
    // We are saving twice in a very short timeframe, once the editor and once the ProFile.
    // So the modification time might not change between those two saves.
    // We manually tell each editor to reload it's file.
    // (The .pro files are notified by the file system watcher.)
    foreach (Core::IEditor *editor, Core::ICore::instance()->editorManager()->editorsForFileName(m_projectFilePath)) {
        if (Core::IFile *editorFile = editor->file()) {
            editorFile->reload(Core::IFile::FlagReload, Core::IFile::TypeContents);
        }
    }

    includeFile->deref();
}

void Qt4PriFileNode::save(const QStringList &lines)
{
    QFile qfile(m_projectFilePath);
    if (qfile.open(QIODevice::WriteOnly | QIODevice::Text)) {
        foreach (const QString &str, lines) {
            qfile.write(str.toLocal8Bit());
            qfile.write("\n");
        }
        qfile.close();
    }

    m_project->qt4ProjectManager()->notifyChanged(m_projectFilePath);
}

/*
  Deletes all subprojects/files/virtual folders
  */
void Qt4PriFileNode::clear()
{
    // delete files && folders && projects
    removeFileNodes(fileNodes(), this);
    removeProjectNodes(subProjectNodes());
    removeFolderNodes(subFolderNodes(), this);
}

QStringList Qt4PriFileNode::varNames(ProjectExplorer::FileType type)
{
    QStringList vars;
    switch (type) {
    case ProjectExplorer::HeaderType:
        vars << QLatin1String("HEADERS");
        vars << QLatin1String("OBJECTIVE_HEADERS");
        break;
    case ProjectExplorer::SourceType:
        vars << QLatin1String("SOURCES");
        vars << QLatin1String("OBJECTIVE_SOURCES");
        vars << QLatin1String("LEXSOURCES");
        vars << QLatin1String("YACCSOURCES");
        break;
    case ProjectExplorer::ResourceType:
        vars << QLatin1String("RESOURCES");
        break;
    case ProjectExplorer::FormType:
        vars << QLatin1String("FORMS");
        break;
    case ProjectExplorer::ProjectFileType:
        vars << QLatin1String("SUBDIRS");
        break;
    case ProjectExplorer::QMLType:
        break;
    default:
        vars << QLatin1String("OTHER_FILES");
        vars << QLatin1String("ICON");
        break;
    }
    return vars;
}


QStringList Qt4PriFileNode::dynamicVarNames(ProFileReader *readerExact, ProFileReader *readerCumulative)
{
    QStringList result;
    // Figure out DEPLOYMENT and INSTALLS
    QStringList listOfVars = readerExact->values("DEPLOYMENT");
    foreach (const QString &var, listOfVars) {
        result << (var + ".sources");
    }
    if (readerCumulative) {
        QStringList listOfVars = readerCumulative->values("DEPLOYMENT");
        foreach (const QString &var, listOfVars) {
            result << (var + ".sources");
        }
    }

    listOfVars = readerExact->values("INSTALLS");
    foreach (const QString &var, listOfVars) {
        result << (var + ".files");
    }
    if (readerCumulative) {
        QStringList listOfVars = readerCumulative->values("INSTALLS");
        foreach (const QString &var, listOfVars) {
            result << (var + ".files");
        }
    }

    return result;
}

QSet<QString> Qt4PriFileNode::filterFiles(ProjectExplorer::FileType fileType, const QSet<QString> &files)
{
    QSet<QString> result;
    if (fileType != ProjectExplorer::QMLType && fileType != ProjectExplorer::UnknownFileType)
        return result;
    if(fileType == ProjectExplorer::QMLType) {
        foreach (const QString &file, files)
            if (file.endsWith(".qml"))
                result << file;
    } else {
        foreach (const QString &file, files)
            if (!file.endsWith(".qml"))
                result << file;
    }
    return result;
}


const Qt4ProFileNode *Qt4ProFileNode::findProFileFor(const QString &fileName) const
{
    if (fileName == path())
        return this;
    foreach (ProjectNode *pn, subProjectNodes())
        if (Qt4ProFileNode *qt4ProFileNode = qobject_cast<Qt4ProFileNode *>(pn))
            if (const Qt4ProFileNode *result = qt4ProFileNode->findProFileFor(fileName))
                return result;
    return 0;
}

TargetInformation Qt4ProFileNode::targetInformation(const QString &fileName) const
{
    TargetInformation result;
    const Qt4ProFileNode *qt4ProFileNode = findProFileFor(fileName);
    if (!qt4ProFileNode)
        return result;

    return qt4ProFileNode->targetInformation();
}

QString Qt4ProFileNode::makefile() const
{
    if (m_varValues[Makefile].isEmpty())
        return QString();
    return m_varValues[Makefile].first();
}

QStringList Qt4ProFileNode::symbianCapabilities() const
{
    QStringList lowerCasedResult;

    QStringList all;
    all << "LocalServices" << "UserEnvironment" << "NetworkServices"
        << "ReadUserData" << "WriteUserData" << "Location" << "SwEvent"
        << "SurroundingsDD" << "ProtServ" << "PowerMgmt" << "ReadDeviceData"
        << "WriteDeviceData" << "TrustedUI" << "NetworkControl"
        << "MultimediaDD"<< "CommDD" << "DiskAdmin" << "AllFiles" << "DRM" << "TCB";

    foreach (const QString &cap, m_varValues[SymbianCapabilities]) {
        QString capability = cap.toLower();
        if (capability.startsWith("-")) {
            lowerCasedResult.removeAll(capability.mid(1));
        } else if (capability == "all") {
            foreach (const QString &a, all)
                if (!lowerCasedResult.contains(a, Qt::CaseInsensitive))
                    lowerCasedResult << a.toLower();
        } else {
            lowerCasedResult << cap;
        }
    }
    QStringList result; //let's make the result pretty
    int index;
    foreach (QString lowerCase, lowerCasedResult) {
        index = all.indexOf(lowerCase);
        if (index != -1)
            result << all.at(index);
        else
            result << lowerCase; //strange capability!
    }
    return result;
}

/*!
  \class Qt4ProFileNode
  Implements abstract ProjectNode class
  */
Qt4ProFileNode::Qt4ProFileNode(Qt4Project *project,
                               const QString &filePath,
                               QObject *parent)
        : Qt4PriFileNode(project, this, filePath),
          m_projectType(InvalidProject),
          m_validParse(false),
          m_readerExact(0),
          m_readerCumulative(0)
{

    if (parent)
        setParent(parent);

    connect(ProjectExplorer::ProjectExplorerPlugin::instance()->buildManager(), SIGNAL(buildStateChanged(ProjectExplorer::Project*)),
            this, SLOT(buildStateChanged(ProjectExplorer::Project*)));

    connect(&m_parseFutureWatcher, SIGNAL(finished()),
            this, SLOT(applyAsyncEvaluate()));
}

Qt4ProFileNode::~Qt4ProFileNode()
{
    CppTools::CppModelManagerInterface *modelManager
            = ExtensionSystem::PluginManager::instance()->getObject<CppTools::CppModelManagerInterface>();
    QMap<QString, Qt4UiCodeModelSupport *>::const_iterator it, end;
    end = m_uiCodeModelSupport.constEnd();
    for (it = m_uiCodeModelSupport.constBegin(); it != end; ++it) {
        modelManager->removeEditorSupport(it.value());
        delete it.value();
    }
    m_parseFutureWatcher.waitForFinished();
    if (m_readerExact) {
        // Oh we need to clean up
        applyEvaluate(true, true);
        m_project->decrementPendingEvaluateFutures();
    }
}

bool Qt4ProFileNode::isParent(Qt4ProFileNode *node)
{
    while ((node = qobject_cast<Qt4ProFileNode *>(node->parentFolderNode()))) {
        if (node == this)
            return true;
    }
    return false;
}

void Qt4ProFileNode::buildStateChanged(ProjectExplorer::Project *project)
{
    if (project == m_project && !ProjectExplorer::ProjectExplorerPlugin::instance()->buildManager()->isBuilding(m_project)) {
        QStringList filesToUpdate = updateUiFiles();
        updateCodeModelSupportFromBuild(filesToUpdate);
    }
}

bool Qt4ProFileNode::hasBuildTargets() const
{
    return hasBuildTargets(projectType());
}

bool Qt4ProFileNode::hasBuildTargets(Qt4ProjectType projectType) const
{
    return (projectType == ApplicationTemplate || projectType == LibraryTemplate);
}

Qt4ProjectType Qt4ProFileNode::projectType() const
{
    return m_projectType;
}

QStringList Qt4ProFileNode::variableValue(const Qt4Variable var) const
{
    return m_varValues.value(var);
}

void Qt4ProFileNode::emitProFileUpdated()
{
    foreach (NodesWatcher *watcher, watchers())
        if (Qt4NodesWatcher *qt4Watcher = qobject_cast<Qt4NodesWatcher*>(watcher))
            emit qt4Watcher->proFileUpdated(this, m_validParse);

    foreach (ProjectNode *subNode, subProjectNodes()) {
        if (Qt4ProFileNode *node = qobject_cast<Qt4ProFileNode *>(subNode)) {
            node->emitProFileUpdated();
        }
    }
}


void Qt4ProFileNode::emitProFileInvalidated()
{
    foreach (NodesWatcher *watcher, watchers())
        if (Qt4NodesWatcher *qt4Watcher = qobject_cast<Qt4NodesWatcher*>(watcher))
            emit qt4Watcher->proFileInvalidated(this);

    foreach (ProjectNode *subNode, subProjectNodes()) {
        if (Qt4ProFileNode *node = qobject_cast<Qt4ProFileNode *>(subNode)) {
            node->emitProFileInvalidated();
        }
    }
}

bool Qt4ProFileNode::validParse() const
{
    return m_validParse;
}

void Qt4ProFileNode::scheduleUpdate()
{
    if (m_validParse) {
        m_validParse = false;
        emitProFileInvalidated();
    }
    m_project->scheduleAsyncUpdate(this);
}

void Qt4ProFileNode::asyncUpdate()
{
    m_project->incrementPendingEvaluateFutures();
    setupReader();
    m_parseFutureWatcher.waitForFinished();
    QFuture<bool> future = QtConcurrent::run(&Qt4ProFileNode::asyncEvaluate, this);
    m_parseFutureWatcher.setFuture(future);
}

void Qt4ProFileNode::update()
{
    if (m_validParse) {
        m_validParse = false;
        foreach (NodesWatcher *watcher, watchers())
            if (Qt4NodesWatcher *qt4Watcher = qobject_cast<Qt4NodesWatcher*>(watcher))
                emit qt4Watcher->proFileInvalidated(this);
    }

    setupReader();
    bool parserError = evaluate();
    applyEvaluate(!parserError, false);
}

void Qt4ProFileNode::setupReader()
{
    Q_ASSERT(!m_readerExact);
    Q_ASSERT(!m_readerCumulative);

    m_readerExact = m_project->createProFileReader(this);
    m_readerExact->setCumulative(false);

    m_readerCumulative = m_project->createProFileReader(this);

    // Find out what flags we pass on to qmake
    QStringList args;
    if (QMakeStep *qs = m_project->activeTarget()->activeBuildConfiguration()->qmakeStep())
        args = qs->parserArguments();
    else
        args = m_project->activeTarget()->activeBuildConfiguration()->configCommandLineArguments();
    m_readerExact->setCommandLineArguments(args);
    m_readerCumulative->setCommandLineArguments(args);
}

bool Qt4ProFileNode::evaluate()
{
    bool parserError = false;
    if (ProFile *pro = m_readerExact->parsedProFile(m_projectFilePath)) {
        if (!m_readerExact->accept(pro, ProFileEvaluator::LoadAll))
            parserError = true;
        if (!m_readerCumulative->accept(pro, ProFileEvaluator::LoadPreFiles))
            parserError = true;
        pro->deref();
    } else {
        parserError = true;
    }
    return parserError;
}

void Qt4ProFileNode::asyncEvaluate(QFutureInterface<bool> &fi)
{
    bool parserError = evaluate();
    fi.reportResult(!parserError);
}

void Qt4ProFileNode::applyAsyncEvaluate()
{
    applyEvaluate(m_parseFutureWatcher.result(), true);
    m_project->decrementPendingEvaluateFutures();
}

static Qt4ProjectType proFileTemplateTypeToProjectType(ProFileEvaluator::TemplateType type)
{
    switch (type) {
    case ProFileEvaluator::TT_Unknown:
    case ProFileEvaluator::TT_Application:
        return ApplicationTemplate;
    case ProFileEvaluator::TT_Library:
        return LibraryTemplate;
    case ProFileEvaluator::TT_Script:
        return ScriptTemplate;
    case ProFileEvaluator::TT_Subdirs:
        return SubDirsTemplate;
    default:
        return InvalidProject;
    }
}

void Qt4ProFileNode::applyEvaluate(bool parseResult, bool async)
{
    if (!m_readerExact)
        return;
    if (!parseResult || m_project->wasEvaluateCanceled()) {
        m_project->destroyProFileReader(m_readerExact);
        if (m_readerCumulative)
            m_project->destroyProFileReader(m_readerCumulative);
        m_readerExact = m_readerCumulative = 0;
        if (!parseResult) {
            m_project->proFileParseError(tr("Error while parsing file %1. Giving up.").arg(m_projectFilePath));
            invalidate();
        }
        foreach (NodesWatcher *watcher, watchers())
            if (Qt4NodesWatcher *qt4Watcher = qobject_cast<Qt4NodesWatcher*>(watcher))
                emit qt4Watcher->proFileUpdated(this, false);
        return;
    }

    if (debug)
        qDebug() << "Qt4ProFileNode - updating files for file " << m_projectFilePath;

    Qt4ProjectType projectType = InvalidProject;
    // Check that both are the same if we have both
    if (m_readerExact->templateType() != m_readerCumulative->templateType()) {
        // Now what. The only thing which could be reasonable is that someone
        // changes between template app and library.
        // Well, we are conservative here for now.
        // Let's wait until someone complains and look at what they are doing.
        m_project->destroyProFileReader(m_readerCumulative);
        m_readerCumulative = 0;
    }

    projectType = proFileTemplateTypeToProjectType(m_readerExact->templateType());

    if (projectType != m_projectType) {
        Qt4ProjectType oldType = m_projectType;
        // probably all subfiles/projects have changed anyway ...
        clear();
        bool changesHasBuildTargets = hasBuildTargets() ^ hasBuildTargets(projectType);

        if (changesHasBuildTargets)
            aboutToChangeHasBuildTargets();

        m_projectType = projectType;

        if (changesHasBuildTargets)
            hasBuildTargetsChanged();

        // really emit here? or at the end? Nobody is connected to this signal at the moment
        // so we kind of can ignore that question for now
        foreach (NodesWatcher *watcher, watchers())
            if (Qt4NodesWatcher *qt4Watcher = qobject_cast<Qt4NodesWatcher*>(watcher))
                emit qt4Watcher->projectTypeChanged(this, oldType, projectType);
    }

    //
    // Add/Remove pri files, sub projects
    //

    QList<ProjectNode*> existingProjectNodes = subProjectNodes();

    QStringList newProjectFilesExact;
    QHash<QString, ProFile*> includeFilesExact;
    ProFile *fileForCurrentProjectExact = 0;
    if (m_projectType == SubDirsTemplate)
        newProjectFilesExact = subDirsPaths(m_readerExact);
    foreach (ProFile *includeFile, m_readerExact->includeFiles()) {
        if (includeFile->fileName() == m_projectFilePath) { // this file
            fileForCurrentProjectExact = includeFile;
        } else {
            newProjectFilesExact << includeFile->fileName();
            includeFilesExact.insert(includeFile->fileName(), includeFile);
        }
    }


    QStringList newProjectFilesCumlative;
    QHash<QString, ProFile*> includeFilesCumlative;
    ProFile *fileForCurrentProjectCumlative = 0;
    if (m_readerCumulative) {
        if (m_projectType == SubDirsTemplate)
            newProjectFilesCumlative = subDirsPaths(m_readerCumulative);
        foreach (ProFile *includeFile, m_readerCumulative->includeFiles()) {
            if (includeFile->fileName() == m_projectFilePath) {
                fileForCurrentProjectCumlative = includeFile;
            } else {
                newProjectFilesCumlative << includeFile->fileName();
                includeFilesCumlative.insert(includeFile->fileName(), includeFile);
            }
        }
    }

    qSort(existingProjectNodes.begin(), existingProjectNodes.end(),
          sortNodesByPath);
    qSort(newProjectFilesExact);
    qSort(newProjectFilesCumlative);

    QList<ProjectNode*> toAdd;
    QList<ProjectNode*> toRemove;

    QList<ProjectNode*>::const_iterator existingIt = existingProjectNodes.constBegin();
    QStringList::const_iterator newExactIt = newProjectFilesExact.constBegin();
    QStringList::const_iterator newCumlativeIt = newProjectFilesCumlative.constBegin();

    forever {
        bool existingAtEnd = (existingIt == existingProjectNodes.constEnd());
        bool newExactAtEnd = (newExactIt == newProjectFilesExact.constEnd());
        bool newCumlativeAtEnd = (newCumlativeIt == newProjectFilesCumlative.constEnd());

        if (existingAtEnd && newExactAtEnd && newCumlativeAtEnd)
            break; // we are done, hurray!

        // So this is one giant loop comparing 3 lists at once and sorting the comparison
        // into mainly 2 buckets: toAdd and toRemove
        // We need to distinguish between nodes that came from exact and cumalative
        // parsing, since the update call is diffrent for them
        // I believe this code to be correct, be careful in changing it

        QString nodeToAdd;
        if (! existingAtEnd
            && (newExactAtEnd || (*existingIt)->path() < *newExactIt)
            && (newCumlativeAtEnd || (*existingIt)->path() < *newCumlativeIt)) {
            // Remove case
            toRemove << *existingIt;
            ++existingIt;
        } else if(! newExactAtEnd
                  && (existingAtEnd || *newExactIt < (*existingIt)->path())
                  && (newCumlativeAtEnd || *newExactIt < *newCumlativeIt)) {
            // Mark node from exact for adding
            nodeToAdd = *newExactIt;
            ++newExactIt;
        } else if (! newCumlativeAtEnd
                   && (existingAtEnd ||  *newCumlativeIt < (*existingIt)->path())
                   && (newExactAtEnd || *newCumlativeIt < *newExactIt)) {
            // Mark node from cumalative for adding
            nodeToAdd = *newCumlativeIt;
            ++newCumlativeIt;
        } else if (!newExactAtEnd
                   && !newCumlativeAtEnd
                   && (existingAtEnd || *newExactIt < (*existingIt)->path())
                   && (existingAtEnd || *newCumlativeIt < (*existingIt)->path())) {
            // Mark node from both for adding
            nodeToAdd = *newExactIt;
            ++newExactIt;
            ++newCumlativeIt;
        } else {
            Q_ASSERT(!newExactAtEnd || !newCumlativeAtEnd);
            // update case, figure out which case exactly
            if (newExactAtEnd) {
                ++newCumlativeIt;
            } else if (newCumlativeAtEnd) {
                ++newExactIt;
            } else if(*newExactIt < *newCumlativeIt) {
                ++newExactIt;
            } else if (*newCumlativeIt < *newExactIt) {
                ++newCumlativeIt;
            } else {
                ++newExactIt;
                ++newCumlativeIt;
            }
            // Update existingNodeIte
            ProFile *fileExact = includeFilesCumlative.value((*existingIt)->path());
            ProFile *fileCumlative = includeFilesCumlative.value((*existingIt)->path());
            if (fileExact || fileCumlative) {
                static_cast<Qt4PriFileNode *>(*existingIt)->update(fileExact, m_readerExact, fileCumlative, m_readerCumulative);
            } else {
                // We always parse exactly, because we later when async parsing don't know whether
                // the .pro file is included in this .pro file
                // So to compare that later parse with the sync one
                if (async)
                    static_cast<Qt4ProFileNode *>(*existingIt)->asyncUpdate();
                else
                    static_cast<Qt4ProFileNode *>(*existingIt)->update();
            }
            ++existingIt;
            // newCumalativeIt and newExactIt are already incremented

        }
        // If we found something to add, do it
        if (!nodeToAdd.isEmpty()) {
            ProFile *fileExact = includeFilesCumlative.value(nodeToAdd);
            ProFile *fileCumlative = includeFilesCumlative.value(nodeToAdd);

            // Loop preventation, make sure that exact same node is not in our parent chain
            bool loop = false;
            ProjectExplorer::Node *n = this;
            while ((n = n->parentFolderNode())) {
                if (qobject_cast<Qt4PriFileNode *>(n) && n->path() == nodeToAdd) {
                    loop = true;
                    break;
                }
            }

            if (loop) {
                // Do nothing
            } else if (fileExact || fileCumlative) {
                Qt4PriFileNode *qt4PriFileNode = new Qt4PriFileNode(m_project, this, nodeToAdd);
                qt4PriFileNode->setParentFolderNode(this); // Needed for loop detection
                qt4PriFileNode->update(fileExact, m_readerExact, fileCumlative, m_readerCumulative);
                toAdd << qt4PriFileNode;
            } else {
                Qt4ProFileNode *qt4ProFileNode = new Qt4ProFileNode(m_project, nodeToAdd);
                qt4ProFileNode->setParentFolderNode(this); // Needed for loop detection
                if (async)
                    qt4ProFileNode->asyncUpdate();
                else
                    qt4ProFileNode->update();
                toAdd << qt4ProFileNode;
            }
        }
    } // for

    if (!toRemove.isEmpty())
        removeProjectNodes(toRemove);
    if (!toAdd.isEmpty())
        addProjectNodes(toAdd);

    Qt4PriFileNode::update(fileForCurrentProjectExact, m_readerExact, fileForCurrentProjectCumlative, m_readerCumulative);

    // update TargetInformation
    m_qt4targetInformation = targetInformation(m_readerExact);

    setupInstallsList(m_readerExact);

    // update other variables
    QHash<Qt4Variable, QStringList> newVarValues;

    newVarValues[DefinesVar] = m_readerExact->values(QLatin1String("DEFINES"));
    newVarValues[IncludePathVar] = includePaths(m_readerExact);
    newVarValues[UiDirVar] = QStringList() << uiDirPath(m_readerExact);
    newVarValues[MocDirVar] = QStringList() << mocDirPath(m_readerExact);
    newVarValues[PkgConfigVar] = m_readerExact->values(QLatin1String("PKGCONFIG"));
    newVarValues[PrecompiledHeaderVar] =
            m_readerExact->absoluteFileValues(QLatin1String("PRECOMPILED_HEADER"),
                                              m_projectDir,
                                              QStringList() << m_projectDir,
                                              0);
    newVarValues[LibDirectoriesVar] = libDirectories(m_readerExact);
    newVarValues[ConfigVar] = m_readerExact->values(QLatin1String("CONFIG"));
    newVarValues[QmlImportPathVar] = m_readerExact->absolutePathValues(
                QLatin1String("QML_IMPORT_PATH"), m_projectDir);
    newVarValues[Makefile] = m_readerExact->values("MAKEFILE");
    // We use the cumulative parse so that we get the capabilities for symbian even if
    // a different target is selected and the capabilities are set in a symbian scope

    newVarValues[SymbianCapabilities] = m_readerCumulative->values("TARGET.CAPABILITY");


    if (m_varValues != newVarValues) {
        m_varValues = newVarValues;

        foreach (NodesWatcher *watcher, watchers())
            if (Qt4NodesWatcher *qt4Watcher = qobject_cast<Qt4NodesWatcher*>(watcher))
                emit qt4Watcher->variablesChanged(this, m_varValues, newVarValues);
    }

    createUiCodeModelSupport();
    updateUiFiles();

    m_validParse = true;

    foreach (NodesWatcher *watcher, watchers())
        if (Qt4NodesWatcher *qt4Watcher = qobject_cast<Qt4NodesWatcher*>(watcher))
            emit qt4Watcher->proFileUpdated(this, parseResult);

    m_project->destroyProFileReader(m_readerExact);
    if (m_readerCumulative)
        m_project->destroyProFileReader(m_readerCumulative);

    m_readerExact = 0;
    m_readerCumulative = 0;
}

namespace {
    // find all ui files in project
    class FindUiFileNodesVisitor : public ProjectExplorer::NodesVisitor {
    public:
        void visitProjectNode(ProjectNode *projectNode)
        {
            visitFolderNode(projectNode);
        }
        void visitFolderNode(FolderNode *folderNode)
        {
            foreach (FileNode *fileNode, folderNode->fileNodes()) {
                if (fileNode->fileType() == ProjectExplorer::FormType)
                    uiFileNodes << fileNode;
            }
        }
        QList<FileNode*> uiFileNodes;
    };
}

// This function is triggered after a build, and updates the state ui files
// It does so by storing a modification time for each ui file we know about.

// TODO this function should also be called if the build directory is changed
QStringList Qt4ProFileNode::updateUiFiles()
{
//    qDebug()<<"Qt4ProFileNode::updateUiFiles()";
    // Only those two project types can have ui files for us
    if (m_projectType != ApplicationTemplate
        && m_projectType != LibraryTemplate)
        return QStringList();

    // Find all ui files
    FindUiFileNodesVisitor uiFilesVisitor;
    this->accept(&uiFilesVisitor);
    const QList<FileNode*> uiFiles = uiFilesVisitor.uiFileNodes;

    // Find the UiDir, there can only ever be one
    QString uiDir = buildDir();
    QStringList tmp = m_varValues[UiDirVar];
    if (tmp.size() != 0)
        uiDir = tmp.first();

    // Collect all existing generated files
    QList<FileNode*> existingFileNodes;
    foreach (FileNode *file, fileNodes()) {
        if (file->isGenerated())
            existingFileNodes << file;
    }

    // Convert uiFile to uiHeaderFilePath, find all headers that correspond
    // and try to find them in uiDir
    QStringList newFilePaths;
    foreach (FileNode *uiFile, uiFiles) {
        const QString uiHeaderFilePath
                = QString("%1/ui_%2.h").arg(uiDir, QFileInfo(uiFile->path()).completeBaseName());
        if (QFileInfo(uiHeaderFilePath).exists())
            newFilePaths << uiHeaderFilePath;
    }

    // Create a diff between those lists
    QList<FileNode*> toRemove;
    QList<FileNode*> toAdd;
    // The list of files for which we call updateSourceFile
    QStringList toUpdate;

    qSort(newFilePaths);
    qSort(existingFileNodes.begin(), existingFileNodes.end(), ProjectNode::sortNodesByPath);

    QList<FileNode*>::const_iterator existingNodeIter = existingFileNodes.constBegin();
    QList<QString>::const_iterator newPathIter = newFilePaths.constBegin();
    while (existingNodeIter != existingFileNodes.constEnd()
           && newPathIter != newFilePaths.constEnd()) {
        if ((*existingNodeIter)->path() < *newPathIter) {
            toRemove << *existingNodeIter;
            ++existingNodeIter;
        } else if ((*existingNodeIter)->path() > *newPathIter) {
            toAdd << new FileNode(*newPathIter, ProjectExplorer::HeaderType, true);
            ++newPathIter;
        } else { // *existingNodeIter->path() == *newPathIter
            QString fileName = (*existingNodeIter)->path();
            QMap<QString, QDateTime>::const_iterator it = m_uitimestamps.find(fileName);
            QDateTime lastModified = QFileInfo(fileName).lastModified();
            if (it == m_uitimestamps.constEnd() || it.value() < lastModified) {
                toUpdate << fileName;
                m_uitimestamps[fileName] = lastModified;
            }
            ++existingNodeIter;
            ++newPathIter;
        }
    }
    while (existingNodeIter != existingFileNodes.constEnd()) {
        toRemove << *existingNodeIter;
        ++existingNodeIter;
    }
    while (newPathIter != newFilePaths.constEnd()) {
        toAdd << new FileNode(*newPathIter, ProjectExplorer::HeaderType, true);
        ++newPathIter;
    }

    // Update project tree
    if (!toRemove.isEmpty()) {
        foreach (FileNode *file, toRemove)
            m_uitimestamps.remove(file->path());
        removeFileNodes(toRemove, this);
    }

    CppTools::CppModelManagerInterface *modelManager =
        ExtensionSystem::PluginManager::instance()->getObject<CppTools::CppModelManagerInterface>();

    if (!toAdd.isEmpty()) {
        foreach (FileNode *file, toAdd) {
            m_uitimestamps.insert(file->path(), QFileInfo(file->path()).lastModified());
            toUpdate << file->path();

            // Also adding files depending on that
            // We only need to do that for files that were newly created
            QString fileName = QFileInfo(file->path()).fileName();
            foreach (CPlusPlus::Document::Ptr doc, modelManager->snapshot()) {
                if (doc->includedFiles().contains(fileName)) {
                    if (!toUpdate.contains(doc->fileName()))
                        toUpdate << doc->fileName();
                }
            }
        }
        addFileNodes(toAdd, this);
    }
    return toUpdate;
}

QString Qt4ProFileNode::uiDirPath(ProFileReader *reader) const
{
    QString path = reader->value("UI_DIR");
    if (QFileInfo(path).isRelative())
        path = QDir::cleanPath(buildDir() + "/" + path);
    return path;
}

QString Qt4ProFileNode::mocDirPath(ProFileReader *reader) const
{
    QString path = reader->value("MOC_DIR");
    if (QFileInfo(path).isRelative())
        path = QDir::cleanPath(buildDir() + "/" + path);
    return path;
}

QStringList Qt4ProFileNode::includePaths(ProFileReader *reader) const
{
    QStringList paths;
    foreach (const QString &cxxflags, m_readerExact->values("QMAKE_CXXFLAGS")) {
        if (cxxflags.startsWith("-I"))
            paths.append(cxxflags.mid(2));
    }

    paths.append(reader->absolutePathValues(QLatin1String("INCLUDEPATH"), m_projectDir));
    // paths already contains moc dir and ui dir, due to corrrectly parsing uic.prf and moc.prf
    // except if those directories don't exist at the time of parsing
    // thus we add those directories manually (without checking for existence)
    paths << mocDirPath(reader) << uiDirPath(reader);
    paths.removeDuplicates();
    return paths;
}

QStringList Qt4ProFileNode::libDirectories(ProFileReader *reader) const
{
    QStringList result;
    foreach (const QString &str, reader->values(QLatin1String("LIBS"))) {
        if (str.startsWith("-L")) {
            result.append(str.mid(2));
        }
    }
    return result;
}

QStringList Qt4ProFileNode::subDirsPaths(ProFileReader *reader) const
{
    QStringList subProjectPaths;

    const QStringList subDirVars = reader->values(QLatin1String("SUBDIRS"));

    foreach (const QString &subDirVar, subDirVars) {
        // Special case were subdir is just an identifier:
        //   "SUBDIR = subid
        //    subid.subdir = realdir"
        // or
        //   "SUBDIR = subid
        //    subid.file = realdir/realfile.pro"

        QString realDir;
        const QString subDirKey = subDirVar + QLatin1String(".subdir");
        const QString subDirFileKey = subDirVar + QLatin1String(".file");
        if (reader->contains(subDirKey))
            realDir = reader->value(subDirKey);
        else if (reader->contains(subDirFileKey))
            realDir = reader->value(subDirFileKey);
        else
            realDir = subDirVar;
        QFileInfo info(realDir);
        if (!info.isAbsolute())
            info.setFile(m_projectDir + QLatin1Char('/') + realDir);
        realDir = info.filePath();

        QString realFile;
        if (info.isDir()) {
            realFile = QString::fromLatin1("%1/%2.pro").arg(realDir, info.fileName());
        } else {
            realFile = realDir;
        }

        if (QFile::exists(realFile)) {
            subProjectPaths << realFile;
        } else {
            m_project->proFileParseError(tr("Could not find .pro file for sub dir '%1' in '%2'")
                                         .arg(subDirVar).arg(realDir));
        }
    }

    subProjectPaths.removeDuplicates();
    return subProjectPaths;
}

TargetInformation Qt4ProFileNode::targetInformation(ProFileReader *reader) const
{
    TargetInformation result;
    if (!reader)
        return result;

    result.buildDir = buildDir();
    const QString baseDir = result.buildDir;
    // qDebug() << "base build dir is:"<<baseDir;

    // Working Directory
    if (reader->contains("DESTDIR")) {
        //qDebug() << "reader contains destdir:" << reader->value("DESTDIR");
        result.workingDir = reader->value("DESTDIR");
        if (QDir::isRelativePath(result.workingDir)) {
            result.workingDir = baseDir + QLatin1Char('/') + result.workingDir;
            //qDebug() << "was relative and expanded to" << result.workingDir;
        }
    } else {
        //qDebug() << "reader didn't contain DESTDIR, setting to " << baseDir;
        result.workingDir = baseDir;
    }

    result.target = reader->value("TARGET");
    if (result.target.isEmpty())
        result.target = QFileInfo(m_projectFilePath).baseName();

#if defined (Q_OS_MAC)
    if (reader->values("CONFIG").contains("app_bundle")) {
        result.workingDir += QLatin1Char('/')
                           + result.target
                           + QLatin1String(".app/Contents/MacOS");
    }
#endif

    result.workingDir = QDir::cleanPath(result.workingDir);

    QString wd = result.workingDir;
    if ( (!reader->contains("DESTDIR") || reader->value("DESTDIR") == ".")
        && reader->values("CONFIG").contains("debug_and_release")
        && reader->values("CONFIG").contains("debug_and_release_target")) {
        // If we don't have a destdir and debug and release is set
        // then the executable is in a debug/release folder
        //qDebug() << "reader has debug_and_release_target";

        // Hmm can we find out whether it's debug or release in a saner way?
        // Theoretically it's in CONFIG
        QString qmakeBuildConfig = "release";
        if (m_project->activeTarget()->activeBuildConfiguration()->qmakeBuildConfiguration() & QtVersion::DebugBuild)
            qmakeBuildConfig = "debug";
        wd += QLatin1Char('/') + qmakeBuildConfig;
    }

    result.executable = QDir::cleanPath(wd + QLatin1Char('/') + result.target);
    //qDebug() << "##### updateTarget sets:" << result.workingDir << result.executable;

#if defined (Q_OS_WIN)
    result.executable += QLatin1String(".exe");
#endif
    result.valid = true;
    return result;
}

TargetInformation Qt4ProFileNode::targetInformation() const
{
    return m_qt4targetInformation;
}

void Qt4ProFileNode::setupInstallsList(const ProFileReader *reader)
{
    m_installsList.clear();
    if (!reader)
        return;
    const QStringList &itemList = reader->values(QLatin1String("INSTALLS"));
    foreach (const QString &item, itemList) {
        QString itemPath;
        const QString pathVar = item + QLatin1String(".path");
        const QStringList &itemPaths = reader->values(pathVar);
        if (itemPaths.count() != 1) {
            qDebug("Invalid RHS: Variable '%s' has %d values.",
                qPrintable(pathVar), itemPaths.count());
            if (itemPaths.isEmpty()) {
                qDebug("%s: Ignoring INSTALLS item '%s', because it has no path.",
                    qPrintable(m_projectFilePath), qPrintable(item));
                continue;
            }
        }
        itemPath = itemPaths.last();

        const QStringList &itemFiles
            = reader->absoluteFileValues(item + QLatin1String(".files"),
                  m_projectDir, QStringList() << m_projectDir, 0);
        if (item == QLatin1String("target")) {
            if (!m_installsList.targetPath.isEmpty())
                qDebug("%s: Overwriting existing target.path in INSTALLS list.",
                    qPrintable(m_projectFilePath));
            m_installsList.targetPath = itemPath;
        } else {
            if (itemFiles.isEmpty()) {
                if (!reader->values(item + QLatin1String(".CONFIG"))
                    .contains(QLatin1String("no_check_exist"))) {
                    qDebug("%s: Ignoring INSTALLS item '%s', because it has no files.",
                        qPrintable(m_projectFilePath), qPrintable(item));
                }
                continue;
            }
            m_installsList.items << InstallsItem(itemPath, itemFiles);
        }
    }
}

InstallsList Qt4ProFileNode::installsList() const
{
    return m_installsList;
}

QString Qt4ProFileNode::buildDir() const
{
    const QDir srcDirRoot = QFileInfo(m_project->rootProjectNode()->path()).absoluteDir();
    const QString relativeDir = srcDirRoot.relativeFilePath(m_projectDir);
    return QDir(m_project->activeTarget()->activeBuildConfiguration()->buildDirectory()).absoluteFilePath(relativeDir);
}

/*
  Sets project type to InvalidProject & deletes all subprojects/files/virtual folders
  */
void Qt4ProFileNode::invalidate()
{
    if (m_projectType == InvalidProject)
        return;

    clear();

    // change project type
    Qt4ProjectType oldType = m_projectType;
    m_projectType = InvalidProject;


    foreach (NodesWatcher *watcher, watchers())
        if (Qt4NodesWatcher *qt4Watcher = qobject_cast<Qt4NodesWatcher*>(watcher))
            emit qt4Watcher->projectTypeChanged(this, oldType, InvalidProject);
}

void Qt4ProFileNode::updateCodeModelSupportFromBuild(const QStringList &files)
{
    foreach (const QString &file, files) {
        QMap<QString, Qt4UiCodeModelSupport *>::const_iterator it, end;
        end = m_uiCodeModelSupport.constEnd();
        for (it = m_uiCodeModelSupport.constBegin(); it != end; ++it) {
            if (it.value()->fileName() == file)
                it.value()->updateFromBuild();
        }
    }
}

void Qt4ProFileNode::updateCodeModelSupportFromEditor(const QString &uiFileName,
                                                      const QString &contents)
{
    const QMap<QString, Qt4UiCodeModelSupport *>::const_iterator it =
            m_uiCodeModelSupport.constFind(uiFileName);
    if (it != m_uiCodeModelSupport.constEnd())
        it.value()->updateFromEditor(contents);
    foreach (ProjectExplorer::ProjectNode *pro, subProjectNodes())
        if (Qt4ProFileNode *qt4proFileNode = qobject_cast<Qt4ProFileNode *>(pro))
            qt4proFileNode->updateCodeModelSupportFromEditor(uiFileName, contents);
}

QString Qt4ProFileNode::uiDirectory() const
{
    const Qt4VariablesHash::const_iterator it = m_varValues.constFind(UiDirVar);
    if (it != m_varValues.constEnd() && !it.value().isEmpty())
        return it.value().front();
    return buildDir();
}

QString Qt4ProFileNode::uiHeaderFile(const QString &uiDir, const QString &formFile)
{
    QString uiHeaderFilePath = uiDir;
    uiHeaderFilePath += QLatin1String("/ui_");
    uiHeaderFilePath += QFileInfo(formFile).completeBaseName();
    uiHeaderFilePath += QLatin1String(".h");
    return QDir::cleanPath(uiHeaderFilePath);
}

void Qt4ProFileNode::createUiCodeModelSupport()
{
//    qDebug()<<"creatUiCodeModelSupport()";
    CppTools::CppModelManagerInterface *modelManager
            = ExtensionSystem::PluginManager::instance()->getObject<CppTools::CppModelManagerInterface>();

    // First move all to
    QMap<QString, Qt4UiCodeModelSupport *> oldCodeModelSupport;
    oldCodeModelSupport = m_uiCodeModelSupport;
    m_uiCodeModelSupport.clear();

    // Only those two project types can have ui files for us
    if (m_projectType == ApplicationTemplate || m_projectType == LibraryTemplate) {
        // Find all ui files
        FindUiFileNodesVisitor uiFilesVisitor;
        this->accept(&uiFilesVisitor);
        const QList<FileNode*> uiFiles = uiFilesVisitor.uiFileNodes;

        // Find the UiDir, there can only ever be one
        const  QString uiDir = uiDirectory();
        foreach (const FileNode *uiFile, uiFiles) {
            const QString uiHeaderFilePath = uiHeaderFile(uiDir, uiFile->path());
//            qDebug()<<"code model support for "<<uiFile->path()<<" "<<uiHeaderFilePath;
            QMap<QString, Qt4UiCodeModelSupport *>::iterator it = oldCodeModelSupport.find(uiFile->path());
            if (it != oldCodeModelSupport.end()) {
//                qDebug()<<"updated old codemodelsupport";
                Qt4UiCodeModelSupport *cms = it.value();
                cms->setFileName(uiHeaderFilePath);
                m_uiCodeModelSupport.insert(it.key(), cms);
                oldCodeModelSupport.erase(it);
            } else {
//                qDebug()<<"adding new codemodelsupport";
                Qt4UiCodeModelSupport *cms = new Qt4UiCodeModelSupport(modelManager, m_project, uiFile->path(), uiHeaderFilePath);
                m_uiCodeModelSupport.insert(uiFile->path(), cms);
                modelManager->addEditorSupport(cms);
            }
        }
    }
    // Remove old
    QMap<QString, Qt4UiCodeModelSupport *>::const_iterator it, end;
    end = oldCodeModelSupport.constEnd();
    for (it = oldCodeModelSupport.constBegin(); it!=end; ++it) {
        modelManager->removeEditorSupport(it.value());
        delete it.value();
    }
}

Qt4NodesWatcher::Qt4NodesWatcher(QObject *parent)
        : NodesWatcher(parent)
{
}

} // namespace Internal
} // namespace Qt4ProjectManager
