/* This file is part of RTags (http://rtags.net).

   RTags is free software: you can redistribute it and/or modify
   it under the terms of the GNU General Public License as published by
   the Free Software Foundation, either version 3 of the License, or
   (at your option) any later version.

   RTags is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
   GNU General Public License for more details.

   You should have received a copy of the GNU General Public License
   along with RTags.  If not, see <http://www.gnu.org/licenses/>. */

#include "Project.h"
#include "FileManager.h"
#include "Diagnostic.h"
#include "IndexerJob.h"
#include "RTags.h"
#include "Server.h"
#include "JobScheduler.h"
#include "RTagsLogOutput.h"
#include <math.h>
#include <fnmatch.h>
#include <rct/Log.h>
#include <rct/MemoryMonitor.h>
#include <rct/Path.h>
#include <rct/Value.h>
#include <rct/Rct.h>
#include <rct/ReadLocker.h>
#include <rct/Thread.h>
#include <rct/DataFile.h>
#include <regex>
#include <memory>
#include "LogOutputMessage.h"

enum { DirtyTimeout = 100 };

// these are externed from Source.cpp
String findSymbolNameByUsr(const std::shared_ptr<Project> &project, uint32_t fileId, const String &usr)
{
    assert(project);
    String ret;
    for (const auto &sym : project->findByUsr(usr, fileId, Project::ArgDependsOn)) {
        ret = sym.symbolName;
        break;
    }
    return ret;
}

Set<Symbol> findTargets(const std::shared_ptr<Project> &project, const Symbol &symbol)
{
    assert(project);
    return project->findTargets(symbol);
}

Set<Symbol> findCallers(const std::shared_ptr<Project> &project, const Symbol &symbol)
{
    assert(project);
    return project->findCallers(symbol);
}

class Dirty
{
public:
    virtual ~Dirty() {}
    virtual Set<uint32_t> dirtied() const = 0;
    virtual bool isDirty(const Source &source) = 0;
};

class SimpleDirty : public Dirty
{
public:
    void init(const Set<uint32_t> &dirty, const std::shared_ptr<Project> &project)
    {
        for (auto fileId : dirty) {
            mDirty.insert(fileId);
            mDirty += project->dependencies(fileId, Project::DependsOnArg);
        }
    }

    virtual Set<uint32_t> dirtied() const override
    {
        return mDirty;
    }

    virtual bool isDirty(const Source &source) override
    {
        return mDirty.contains(source.fileId);
    }

    Set<uint32_t> mDirty;
};

class ComplexDirty : public Dirty
{
public:
    virtual Set<uint32_t> dirtied() const override
    {
        return mDirty;
    }
    void insertDirtyFile(uint32_t fileId)
    {
        mDirty.insert(fileId);
    }
    inline uint64_t lastModified(uint32_t fileId)
    {
        uint64_t &time = mLastModified[fileId];
        if (!time) {
            time = Location::path(fileId).lastModifiedMs();
        }
        return time;
    }

    Hash<uint32_t, uint64_t> mLastModified;
    Set<uint32_t> mDirty;
};

class SuspendedDirty : public ComplexDirty
{
public:
    bool isDirty(const Source &)
    {
        return false;
    }
};

class IfModifiedDirty : public ComplexDirty
{
public:
    IfModifiedDirty(const std::shared_ptr<Project> &project, const Match &match = Match())
        : mProject(project), mMatch(match)
    {
    }

    virtual bool isDirty(const Source &source) override
    {
        bool ret = false;

        if (mMatch.isEmpty() || mMatch.match(source.sourceFile())) {
            for (auto it : mProject->dependencies(source.fileId, Project::ArgDependsOn)) {
                const uint64_t depLastModified = lastModified(it);
                if (!depLastModified || depLastModified > source.parsed) {
                    // dependency is gone
                    ret = true;
                    insertDirtyFile(it);
                }
            }
            if (ret)
                mDirty.insert(source.fileId);

            assert(!ret || mDirty.contains(source.fileId));
        }
        return ret;
    }

    std::shared_ptr<Project> mProject;
    Match mMatch;
};


class WatcherDirty : public ComplexDirty
{
public:
    WatcherDirty(const std::shared_ptr<Project> &project, const Set<uint32_t> &modified)
    {
        for (auto it : modified) {
            mModified[it] = project->dependencies(it, Project::DependsOnArg);
        }
    }

    virtual bool isDirty(const Source &source) override
    {
        bool ret = false;

        for (auto it : mModified) {
            const auto &deps = it.second;
            if (deps.contains(source.fileId)) {
                const uint64_t depLastModified = lastModified(it.first);
                if (!depLastModified || depLastModified > source.parsed) {
                    // dependency is gone
                    ret = true;
                    insertDirtyFile(it.first);
                }
            }
        }

        if (ret)
            insertDirtyFile(source.fileId);
        return ret;
    }

    Hash<uint32_t, Set<uint32_t> > mModified;
};

static void loadDependencies(DataFile &file, Dependencies &dependencies)
{
    int size;
    file >> size;
    for (int i=0; i<size; ++i) {
        uint32_t fileId;
        file >> fileId;
        dependencies[fileId] = new DependencyNode(fileId);
    }
    for (int i=0; i<size; ++i) {
        int links;
        file >> links;
        if (links) {
            uint32_t dependee;
            file >> dependee;
            DependencyNode *ee = dependencies[dependee];
            assert(ee);
            for (int i=0; i<links; ++i) {
                uint32_t dependent;
                file >> dependent;
                DependencyNode *ent = dependencies[dependent];
                assert(ent);
                ent->include(ee);
            }
        }
    }
}

static void saveDependencies(DataFile &file, const Dependencies &dependencies)
{
    file << dependencies.size();
    for (const auto &it : dependencies) {
        file << it.first;
    }
    for (const auto &it : dependencies) {
        file << it.second->dependents.size();
        if (!it.second->dependents.isEmpty()) {
            file << it.first;
            for (const auto &dep : it.second->dependents) {
                file << dep.first;
            }
        }
    }
}

Project::Project(const Path &path)
    : mPath(path), mSourceFilePathBase(RTags::encodeSourceFilePath(Server::instance()->options().dataDir, path)),
      mJobCounter(0), mJobsStarted(0)
{
    Path srcPath = mPath;
    RTags::encodePath(srcPath);
    const Server::Options &options = Server::instance()->options();
    const Path tmp = options.dataDir + srcPath;
    mProjectFilePath = tmp + "/project";
    mSourcesFilePath = tmp + "/sources";
}

Project::~Project()
{
    for (const auto &job : mActiveJobs) {
        assert(job.second);
        Server::instance()->jobScheduler()->abort(job.second);
    }
    mDependencies.deleteAll();

    assert(EventLoop::isMainThread());
    mDirtyTimer.stop();
}

static bool hasSourceDependency(const DependencyNode *node, const std::shared_ptr<Project> &project, Set<uint32_t> &seen)
{
    const Path path = Location::path(node->fileId);
    // error("%s %d %d", path.constData(), path.isFile(), path.isSource());
    if (path.isFile() && path.isSource() && project->hasSource(node->fileId)) {
        return true;
    }
    for (auto it : node->dependents) {
        if (seen.insert(it.first) && hasSourceDependency(it.second, project, seen))
            return true;
    }
    return false;
}

static inline bool hasSourceDependency(const DependencyNode *node, const std::shared_ptr<Project> &project)
{
    Set<uint32_t> seen;
    return hasSourceDependency(node, project, seen);
}

bool Project::readSources(const Path &path, Sources &sources, String *err)
{
    DataFile file(path, RTags::SourcesFileVersion);
    if (!file.open(DataFile::Read)) {
        Path::rm(path);
        if (err && !file.error().isEmpty())
            *err = file.error();
        return false;
    }

    file >> sources;
    return true;
}

bool Project::init()
{
    const Server::Options &options = Server::instance()->options();
    if (!(options.options & Server::NoFileSystemWatch)) {
        mWatcher.modified().connect(std::bind(&Project::onFileModified, this, std::placeholders::_1));
        mWatcher.added().connect(std::bind(&Project::onFileAdded, this, std::placeholders::_1));
        mWatcher.removed().connect(std::bind(&Project::onFileRemoved, this, std::placeholders::_1));
    }
    mFileManager.reset(new FileManager(shared_from_this()));
    mWatcher.removed().connect(std::bind(&FileManager::onFileRemoved, mFileManager.get(), std::placeholders::_1));
    mWatcher.added().connect(std::bind(&FileManager::onFileAdded, mFileManager.get(), std::placeholders::_1));

    mDirtyTimer.timeout().connect(std::bind(&Project::onDirtyTimeout, this, std::placeholders::_1));

    String err;
    if (!Project::readSources(mSourcesFilePath, mSources, &err)) {
        if (!err.isEmpty())
            error("Sources restore error %s: %s", mPath.constData(), err.constData());

        return false;
    }

    DataFile file(mProjectFilePath, RTags::DatabaseVersion);
    if (!file.open(DataFile::Read)) {
        if (!file.error().isEmpty())
            error("Restore error %s: %s", mPath.constData(), file.error().constData());
        Path::rm(mProjectFilePath);
        for (const auto &source : mSources) {
            index(std::shared_ptr<IndexerJob>(new IndexerJob(source.second, IndexerJob::Compile, shared_from_this())));
        }
        return true;
    }

    {
        std::lock_guard<std::mutex> lock(mMutex);
        file >> mVisitedFiles >> mDiagnostics;
    }
    file >> mDeclarations;
    loadDependencies(file, mDependencies);

    for (const auto &dep : mDependencies) {
        watchFile(dep.first);
    }

    bool needsSave = false;
    std::unique_ptr<ComplexDirty> dirty;

    if (Server::instance()->suspended()) {
        dirty.reset(new SuspendedDirty);
    } else {
        dirty.reset(new IfModifiedDirty(shared_from_this()));
    }

    Set<uint32_t> missingFileMaps;
    {
        List<uint32_t> removed;
        int idx = 0;
        bool outputDirty = false;
        if (mDependencies.size() >= 100) {
            logDirect(LogLevel::Error, String::format<128>("Restoring %s ", mPath.constData()), Flags<LogOutput::LogFlag>());
            outputDirty = true;
        }
        const std::shared_ptr<Project> project = shared_from_this();
        for (auto it : mDependencies) {
            const Path path = Location::path(it.first);
            if (!path.isFile()) {
                warning() << path << "seems to have disappeared";
                dirty.get()->insertDirtyFile(it.first);

                const Set<uint32_t> dependents = dependencies(it.first, DependsOnArg);
                for (auto dependent : dependents) {
                    dirty.get()->insertDirtyFile(dependent);
                }
                removed << it.first;
                needsSave = true;
            } else {
                String err;
                if (!validate(it.first, &err)) {
                    if (!err.isEmpty()) {
                        if (outputDirty) {
                            outputDirty = false;
                            error("\n");
                        }
                        error() << err;
                    }
                    if (hasSource(it.first) || hasSourceDependency(it.second, project)) {
                        missingFileMaps.insert(it.first);
                    } else {
                        removed << it.first;
                        needsSave = true;
                    }
                }
            }
            if (++idx % 100 == 0) {
                outputDirty = true;
                logDirect(LogLevel::Error, ".", 1, Flags<LogOutput::LogFlag>());
                // error("%d/%d (%.2f%%)", idx, count, (idx / static_cast<double>(count)) * 100.0);
            }
        }
        if (outputDirty)
            logDirect(LogLevel::Error, "\n", 1, Flags<LogOutput::LogFlag>());
        for (uint32_t r : removed) {
            removeDependencies(r);
        }
    }

    auto it = mSources.begin();
    while (it != mSources.end()) {
        const Source &source = it->second;
        const Path sourceFile = source.sourceFile();
        if (!sourceFile.isFile()) {
            warning() << source.sourceFile() << "seems to have disappeared";
            removeDependencies(source.fileId);
            dirty.get()->insertDirtyFile(source.fileId);
            mSources.erase(it++);
            needsSave = true;
        } else {
            watchFile(source.fileId);
            ++it;
        }
    }

    if (needsSave)
        save();
    startDirtyJobs(dirty.get());
    if (!missingFileMaps.isEmpty()) {
        SimpleDirty simple;
        simple.init(missingFileMaps, shared_from_this());
        startDirtyJobs(&simple);
    }
    return true;
}

bool Project::match(const Match &p, bool *indexed) const
{
    Path paths[] = { p.pattern(), p.pattern() };
    paths[1].resolve();
    const int count = paths[1].compare(paths[0]) ? 2 : 1;
    bool ret = false;
    const Path resolvedPath = mPath.resolved();
    for (int i=0; i<count; ++i) {
        const Path &path = paths[i];
        const uint32_t id = Location::fileId(path);
        if (id && isIndexed(id)) {
            if (indexed)
                *indexed = true;
            return true;
        } else if (mFiles.contains(path) || p.match(mPath) || p.match(resolvedPath)) {
            if (!indexed)
                return true;
            ret = true;
        }
    }
    if (indexed)
        *indexed = false;
    return ret;
}

enum DiagnosticsFormat {
    Diagnostics_XML,
    Diagnostics_Elisp
};

static inline bool remove(Diagnostics &diags, uint32_t file)
{
    Diagnostics::iterator it = diags.lower_bound(Location(file, 0, 0));
    bool ret = false;
    while (it != diags.end() && it->first.fileId() == file) {
        diags.erase(it++);
        ret = true;
    }
    return ret;
}

struct FormatDiagnosticsOperation {
    DiagnosticsFormat format;
    Diagnostics newDiags;
    uint32_t fileId;
    bool all;
};

static String formatDiagnostics(const FormatDiagnosticsOperation &op, Diagnostics &diagnostics)
{
    const char *severities[] = { "none", "warning", "error", "fixit", "skipped" };

    static const char *header[] = {
        "<?xml version=\"1.0\" encoding=\"utf-8\"?>\n  <checkstyle>",
        "(list 'checkstyle "
    };
    static const char *fileEmpty[] = {
        "\n    <file name=\"%s\">\n    </file>",
        "(cons \"%s\" nil)"
    };
    static const char *startFile[] = {
        "\n    <file name=\"%s\">",
        "(cons \"%s\" (list"
    };
    static const char *endFile[] = {
        "\n    </file>",
        "))"
    };
    static const char *trailer[] = {
        "\n  </checkstyle>",
        ")"
    };
    std::function<String(const Location &, const Diagnostic &)> formatDiagnostic;
    if (op.format == Diagnostics_XML) {
        formatDiagnostic = [severities](const Location &loc, const Diagnostic &diagnostic) {
            return String::format<256>("\n      <error line=\"%d\" column=\"%d\" %sseverity=\"%s\" message=\"%s\"/>",
                                       loc.line(), loc.column(),
                                       (diagnostic.length <= 0 ? ""
                                        : String::format<32>("length=\"%d\" ", diagnostic.length).constData()),
                                       severities[diagnostic.type], RTags::xmlEscape(diagnostic.message).constData());
        };
    } else {
        formatDiagnostic = [severities](const Location &loc, const Diagnostic &diagnostic) {
            return String::format<256>(" (list %d %d %s '%s \"%s\")",
                                       loc.line(), loc.column(),
                                       diagnostic.length > 0 ? String::number(diagnostic.length).constData() : "nil",
                                       severities[diagnostic.type],
                                       RTags::elispEscape(diagnostic.message).constData());
        };
    }
    if (op.fileId) {
        String ret;
        assert(op.newDiags.isEmpty());
        assert(!op.all);
        const Path path = Location::path(op.fileId);
        ret << header[op.format]
            << String::format<256>(startFile[op.format], path.constData());

        Diagnostics::iterator it = diagnostics.lower_bound(Location(op.fileId, 0, 0));
        bool found = false;
        while (it != diagnostics.end() && it->first.fileId() == op.fileId) {
            found = true;
            ret << formatDiagnostic(it->first, it->second);
            ++it;
        }
        if (!found) {
            ret << String::format<256>(fileEmpty[op.format], path.constData());
        }
        ret << endFile[op.format] << trailer[op.format];
        return ret;
    }

    if (op.all) {
        String ret;
        uint32_t lastFileId = 0;
        bool first = true;
        for (const auto &entry : diagnostics) {
            const Location &loc = entry.first;
            const Diagnostic &diagnostic = entry.second;
            if (loc.fileId() != lastFileId) {
                if (first) {
                    ret = header[op.format];
                    first = false;
                }
                if (lastFileId)
                    ret << endFile[op.format];
                lastFileId = loc.fileId();
                ret << String::format<256>(startFile[op.format], loc.path().constData());
            }
            diagnostics[entry.first] = entry.second;
            ret << formatDiagnostic(loc, diagnostic);
        }
        if (lastFileId)
            ret << endFile[op.format];
        if (!first)
            ret << trailer[op.format];
        return ret;
    }

    String ret;
    bool first = true;
    uint32_t lastFileId = 0;
    for (const auto &entry : op.newDiags) {
        const Location &loc = entry.first;
        const Diagnostic &diagnostic = entry.second;
        if (diagnostic.type == Diagnostic::None) {
            if (lastFileId) {
                ret << endFile[op.format];
                lastFileId = 0;
            }
            if (::remove(diagnostics, entry.first.fileId())) {
                if (first) {
                    ret = header[op.format];
                    first = false;
                }
                ret << String::format<256>(fileEmpty[op.format], loc.path().constData());
            }
        } else {
            if (loc.fileId() != lastFileId) {
                ::remove(diagnostics, entry.first.fileId());
                if (first) {
                    ret = header[op.format];
                    first = false;
                }
                if (lastFileId)
                    ret << endFile[op.format];
                lastFileId = loc.fileId();
                ret << String::format<256>(startFile[op.format], loc.path().constData());
            }
            diagnostics[entry.first] = entry.second;
            ret << formatDiagnostic(loc, diagnostic);
        }
    }
    if (lastFileId)
        ret << endFile[op.format];
    if (!first)
        ret << trailer[op.format];
    return ret;
}

void Project::onJobFinished(const std::shared_ptr<IndexerJob> &job, const std::shared_ptr<IndexDataMessage> &msg)
{
    std::shared_ptr<IndexerJob> restart;
    const uint32_t fileId = msg->fileId();
    auto j = mActiveJobs.take(msg->key());
    if (!j) {
        error() << "Couldn't find JobData for" << Location::path(fileId) << msg->key() << job->id << job.get();
        return;
    } else if (j != job) {
        error() << "Wrong IndexerJob for" << Location::path(fileId) << msg->key() << job->id << job.get();
        return;
    }

    const bool success = job->flags & IndexerJob::Complete;
    assert(!(job->flags & IndexerJob::Aborted));
    assert(((job->flags & (IndexerJob::Complete|IndexerJob::Crashed)) == IndexerJob::Complete)
           || ((job->flags & (IndexerJob::Complete|IndexerJob::Crashed)) == IndexerJob::Crashed));
    const auto &options = Server::instance()->options();
    if (!success) {
        releaseFileIds(job->visited);
    }

    auto src = mSources.find(msg->key());
    if (src == mSources.end()) {
        releaseFileIds(job->visited);
        error() << "Can't find source for" << Location::path(fileId);
        return;
    }
    if (!(msg->flags() & IndexDataMessage::ParseFailure)) {
        for (uint32_t fileId : job->visited) {
            if (!validate(fileId)) {
                releaseFileIds(job->visited);
                dirty(job->source.fileId);
                return;
            }
        }
    }

    const int idx = mJobCounter - mActiveJobs.size();
    if (!msg->diagnostics().isEmpty() || options.options & Server::Progress) {
        log([&](const std::shared_ptr<LogOutput> &output) {
                if (output->testLog(RTags::Diagnostics)) {
                    DiagnosticsFormat format = Diagnostics_XML;
                    if (output->flags() & RTagsLogOutput::Elisp) {
                        // I know this is RTagsLogOutput because it returned
                        // true for testLog(RTags::Diagnostics)
                        format = Diagnostics_Elisp;
                    }
                    if (!msg->diagnostics().isEmpty()) {
                        const FormatDiagnosticsOperation op = { format, msg->diagnostics(), 0, false };
                        const String log = formatDiagnostics(op, mDiagnostics);
                        if (!log.isEmpty()) {
                            output->log(log);
                        }
                    }
                    if (options.options & Server::Progress) {
                        if (format == Diagnostics_XML) {
                            output->log("<?xml version=\"1.0\" encoding=\"utf-8\"?>\n<progress index=\"%d\" total=\"%d\"></progress>",
                                        idx, mJobCounter);
                        } else {
                            output->log("(list 'progress %d %d)", idx, mJobCounter);
                        }
                    }
                }
            });
    }

    int symbolNames = 0;
    Set<uint32_t> visited = msg->visitedFiles();
    updateFixIts(visited, msg->fixIts());
    updateDependencies(msg);
    updateDeclarations(visited, msg->declarations());
    if (success) {
        src->second.parsed = msg->parseTime();
        error("[%3d%%] %d/%d %s %s. (%s)",
              static_cast<int>(round((double(idx) / double(mJobCounter)) * 100.0)), idx, mJobCounter,
              String::formatTime(time(0), String::Time).constData(),
              msg->message().constData(),
              (job->priority == IndexerJob::HeaderError
               ? "header-error"
               : String::format<16>("priority %d", job->priority).constData()));
    } else {
        assert(msg->indexerJobFlags() & IndexerJob::Crashed);
        error("[%3d%%] %d/%d %s %s indexing crashed.",
              static_cast<int>(round((double(idx) / double(mJobCounter)) * 100.0)), idx, mJobCounter,
              String::formatTime(time(0), String::Time).constData(),
              Location::path(fileId).toTilde().constData());
    }

    save();
    if (mActiveJobs.isEmpty()) {
        double timerElapsed = (mTimer.elapsed() / 1000.0);
        const double averageJobTime = timerElapsed / mJobsStarted;
        const String msg = String::format<1024>("Jobs took %.2fs%s. We're using %lldmb of memory. ",
                                                timerElapsed, mJobsStarted > 1 ? String::format(", (avg %.2fs)", averageJobTime).constData() : "",
                                                MemoryMonitor::usage() / (1024 * 1024), symbolNames);
        error() << msg;
        mJobsStarted = mJobCounter = 0;

        // error() << "Finished this
    }
}

void Project::diagnose(uint32_t fileId)
{
    log([&](const std::shared_ptr<LogOutput> &output) {
            if (output->testLog(RTags::Diagnostics)) {
                DiagnosticsFormat format = Diagnostics_XML;
                if (output->flags() & RTagsLogOutput::Elisp) {
                    // I know this is RTagsLogOutput because it returned
                    // true for testLog(RTags::Diagnostics)
                    format = Diagnostics_Elisp;
                }
                const FormatDiagnosticsOperation op = { format, Diagnostics(), fileId, false } ;
                const String log = formatDiagnostics(op, mDiagnostics);
                if (!log.isEmpty())
                    output->log(log);
            }
        });
}

void Project::diagnoseAll()
{
    log([&](const std::shared_ptr<LogOutput> &output) {
            if (output->testLog(RTags::Diagnostics)) {
                DiagnosticsFormat format = Diagnostics_XML;
                if (output->flags() & RTagsLogOutput::Elisp) {
                    // I know this is RTagsLogOutput because it returned
                    // true for testLog(RTags::Diagnostics)
                    format = Diagnostics_Elisp;
                }
                const FormatDiagnosticsOperation op = { format, Diagnostics(), 0, true } ;
                const String log = formatDiagnostics(op, mDiagnostics);
                if (!log.isEmpty())
                    output->log(log);
            }
        });
}

bool Project::save()
{
    {
        DataFile file(mSourcesFilePath, RTags::SourcesFileVersion);
        if (!file.open(DataFile::Write)) {
            error("Save error %s: %s", mProjectFilePath.constData(), file.error().constData());
            return false;
        }
        file << mSources;
    }

    {
        DataFile file(mProjectFilePath, RTags::DatabaseVersion);
        if (!file.open(DataFile::Write)) {
            error("Save error %s: %s", mProjectFilePath.constData(), file.error().constData());
            return false;
        }
        {
            std::lock_guard<std::mutex> lock(mMutex);
            file << mVisitedFiles << mDiagnostics;
        }
        file << mDeclarations;
        saveDependencies(file, mDependencies);
        if (!file.flush()) {
            error("Save error %s: %s", mProjectFilePath.constData(), file.error().constData());
            return false;
        }
    }

    return true;
}

static inline void markActive(Sources::iterator start, uint32_t buildId, const Sources::iterator end)
{
    const uint32_t fileId = start->second.fileId;
    while (start != end) {
        uint32_t f, b;
        Source::decodeKey(start->first, f, b);
        if (f != fileId)
            break;

        if (b == buildId) {
            start->second.flags |= Source::Active;
        } else {
            start->second.flags &= ~Source::Active;
        }
        ++start;
    }
}


void Project::index(const std::shared_ptr<IndexerJob> &job)
{
    const Path sourceFile = job->sourceFile;
    static const char *fileFilter = getenv("RTAGS_FILE_FILTER");
    if (fileFilter && !strstr(job->sourceFile.constData(), fileFilter)) {
        error() << "Not indexing" << job->sourceFile.constData() << "because of file filter"
                << fileFilter;
        return;
    }

    const uint64_t key = job->source.key();
    if (Server::instance()->suspended() && mSources.contains(key) && (job->flags & IndexerJob::Compile)) {
        return;
    }

    if (job->flags & IndexerJob::Compile) {
        const auto &options = Server::instance()->options();
        if (options.options & Server::NoFileSystemWatch) {
            auto it = mSources.lower_bound(Source::key(job->source.fileId, 0));
            if (it != mSources.end()) {
                uint32_t f, b;
                Source::decodeKey(it->first, f, b);
                if (f == job->source.fileId) {
                    // When we're not watching the file system, we ignore
                    // updating compiles. This means that you always have to
                    // do check-reindex to build existing files!
                    return;
                }
            }
        } else {
            auto cur = mSources.find(key);
            if (cur != mSources.end()) {
                if (!(cur->second.flags & Source::Active))
                    markActive(mSources.lower_bound(Source::key(job->source.fileId, 0)), cur->second.buildRootId, mSources.end());
                if (cur->second.compareArguments(job->source)) {
                    // no updates
                    return;
                }
            } else {
                auto it = mSources.lower_bound(Source::key(job->source.fileId, 0));
                const auto start = it;
                const bool disallowMultiple = options.options & Server::DisallowMultipleSources;
                bool unsetActive = false;
                while (it != mSources.end()) {
                    uint32_t f, b;
                    Source::decodeKey(it->first, f, b);
                    if (f != job->source.fileId)
                        break;

                    if (it->second.compareArguments(job->source)) {
                        markActive(start, b, mSources.end());
                        // no updates
                        return;
                    } else if (disallowMultiple) {
                        mSources.erase(it++);
                        continue;
                    }
                    unsetActive = true;
                    ++it;
                }
                if (unsetActive) {
                    assert(!disallowMultiple);
                    markActive(start, 0, mSources.end());
                }
            }
        }
    }

    Source &src = mSources[key];
    src = job->source;
    src.flags |= Source::Active;

    std::shared_ptr<IndexerJob> &ref = mActiveJobs[key];
    if (ref) {
        releaseFileIds(ref->visited);
        Server::instance()->jobScheduler()->abort(ref);
        --mJobCounter;
    }
    ref = job;

    ++mJobsStarted;
    if (!mJobCounter++) {
        mTimer.start();
    }

    Server::instance()->jobScheduler()->add(job);
}

void Project::onFileModified(const Path &path)
{
    debug() << path << "was modified";
    onFileAddedOrModified(path);
}

void Project::onFileAdded(const Path &path)
{
    debug() << path << "was added";
    onFileAddedOrModified(path);
}

void Project::onFileAddedOrModified(const Path &file)
{
    const uint32_t fileId = Location::fileId(file);
    debug() << file << "was modified" << fileId;
    if (!fileId)
        return;
    if (Server::instance()->suspended() || mSuspendedFiles.contains(fileId)) {
        warning() << file << "is suspended. Ignoring modification";
        return;
    }
    Server::instance()->jobScheduler()->clearHeaderError(fileId);
    if (mPendingDirtyFiles.insert(fileId)) {
        mDirtyTimer.restart(DirtyTimeout, Timer::SingleShot);
    }
}

void Project::onFileRemoved(const Path &file)
{
    const uint32_t fileId = Location::fileId(file);
    debug() << file << "was removed" << fileId;
    if (!fileId)
        return;
    Rct::removeDirectory(Project::sourceFilePath(fileId));

    const uint64_t key = Source::key(fileId, 0);
    auto it = mSources.lower_bound(key);
    bool needSave = false;
    while (it != mSources.end()) {
        uint32_t f, b;
        Source::decodeKey(it->first, f, b);
        if (f != fileId)
            break;
        needSave = true;
        auto job = mActiveJobs.take(it->first);
        if (job) {
            releaseFileIds(job->visited);
            Server::instance()->jobScheduler()->abort(job);
        }
        debug() << "Erasing source" << Location::path(f);
        mSources.erase(it++);
    }

    if (needSave)
        save();
    Server::instance()->jobScheduler()->clearHeaderError(fileId);

    if (Server::instance()->suspended() || mSuspendedFiles.contains(fileId)) {
        warning() << file << "is suspended. Ignoring modification";
        return;
    }
    if (mPendingDirtyFiles.insert(fileId)) {
        mDirtyTimer.restart(DirtyTimeout, Timer::SingleShot);
    }
}

void Project::onDirtyTimeout(Timer *)
{
    Set<uint32_t> dirtyFiles = std::move(mPendingDirtyFiles);
    WatcherDirty dirty(shared_from_this(), dirtyFiles);
    const int dirtied = startDirtyJobs(&dirty);
    debug() << "onDirtyTimeout" << dirtyFiles << dirtied;
}

List<Source> Project::sources(uint32_t fileId) const
{
    List<Source> ret;
    if (fileId) {
        auto it = mSources.lower_bound(Source::key(fileId, 0));
        while (it != mSources.end()) {
            uint32_t f, b;
            Source::decodeKey(it->first, f, b);
            if (f != fileId)
                break;
            ret.append(it->second);
            ++it;
        }
    }
    return ret;
}

bool Project::hasSource(uint32_t fileId) const
{
    auto it = mSources.lower_bound(Source::key(fileId, 0));
    while (it != mSources.end()) {
        uint32_t f, b;
        Source::decodeKey(it->first, f, b);
        return f == fileId;
    }
    return false;
}

Set<uint32_t> Project::dependencies(uint32_t fileId, DependencyMode mode) const
{
    Set<uint32_t> ret;
    ret.insert(fileId);
    std::function<void(uint32_t)> fill = [&](uint32_t fileId) {
        if (DependencyNode *node = mDependencies.value(fileId)) {
            const auto &nodes = (mode == ArgDependsOn ? node->includes : node->dependents);
            for (const auto &it : nodes) {
                if (ret.insert(it.first))
                    fill(it.first);
            }
        }
    };
    fill(fileId);
    return ret;
}

bool Project::dependsOn(uint32_t source, uint32_t header) const
{
    Set<uint32_t> seen;
    std::function<bool(DependencyNode *node)> dep = [&](DependencyNode *node) {
        assert(node);
        if (!seen.insert(node->fileId))
            return false;
        if (node->dependents.contains(source))
            return true;
        for (const std::pair<uint32_t, DependencyNode*> &n : node->dependents) {
            if (dep(n.second))
                return true;
        }
        return false;
    };
    DependencyNode *node = mDependencies.value(header);
    return node && dep(node);
}

void Project::removeDependencies(uint32_t fileId)
{
    if (DependencyNode *node = mDependencies.take(fileId)) {
        for (auto it : node->includes)
            it.second->dependents.remove(fileId);
        for (auto it : node->dependents)
            it.second->includes.remove(fileId);
        delete node;
    }
}

void Project::updateDependencies(const std::shared_ptr<IndexDataMessage> &msg)
{
    const bool prune = !(msg->flags() & (IndexDataMessage::InclusionError|IndexDataMessage::ParseFailure));
    Set<uint32_t> files;
    for (auto pair : msg->files()) {
        DependencyNode *&node = mDependencies[pair.first];
        if (!node) {
            node = new DependencyNode(pair.first);
            if (pair.second & IndexDataMessage::Visited)
                files.insert(pair.first);
        } else if (pair.second & IndexDataMessage::Visited) {
            files.insert(pair.first);
            if (prune) {
                for (auto it : node->includes)
                    it.second->dependents.remove(pair.first);
                node->includes.clear();
            }
        }
        watchFile(pair.first);
    }

    // // ### this probably deletes and recreates the same nodes very very often
    for (auto it : msg->includes()) {
        DependencyNode *&includer = mDependencies[it.first];
        DependencyNode *&inclusiary = mDependencies[it.second];
        files.insert(it.first);
        files.insert(it.second);
        if (!includer)
            includer = new DependencyNode(it.first);
        if (!inclusiary)
            inclusiary = new DependencyNode(it.second);
        includer->include(inclusiary);
    }
}

void Project::updateDeclarations(const Set<uint32_t> &visited, Declarations &declarations)
{
    auto it = mDeclarations.begin();
    while (it != mDeclarations.end()) {
        if (it->second.remove([&visited](uint32_t key) { return visited.contains(key); }) && it->second.isEmpty()) {
            mDeclarations.erase(it++);
        } else {
            ++it;
        }
    }
    for (auto &u : declarations) {
        auto &cur = mDeclarations[u.first];
        if (cur.isEmpty()) {
            cur = std::move(u.second);
        } else {
            cur.unite(u.second);
        }
    }
}

int Project::reindex(const Match &match,
                     const std::shared_ptr<QueryMessage> &query,
                     const std::shared_ptr<Connection> &wait)
{
    if (query->type() == QueryMessage::Reindex) {
        Set<uint32_t> dirtyFiles;

        const auto end = mDependencies.constEnd();
        for (auto it = mDependencies.constBegin(); it != end; ++it) {
            if (!dirtyFiles.contains(it->first) && (match.isEmpty() || match.match(Location::path(it->first)))) {
                dirtyFiles.insert(it->first);
            }
        }
        if (dirtyFiles.isEmpty())
            return 0;
        SimpleDirty dirty;
        dirty.init(dirtyFiles, shared_from_this());
        return startDirtyJobs(&dirty, query->unsavedFiles(), wait);
    } else {
        assert(query->type() == QueryMessage::CheckReindex);
        IfModifiedDirty dirty(shared_from_this(), match);
        return startDirtyJobs(&dirty, query->unsavedFiles(), wait);
    }
}

int Project::remove(const Match &match)
{
    int count = 0;
    auto it = mSources.begin();
    while (it != mSources.end()) {
        if (match.match(it->second.sourceFile())) {
            const uint32_t fileId = it->second.fileId;
            const uint64_t key = it->first;
            mSources.erase(it++);
            std::shared_ptr<IndexerJob> job = mActiveJobs.take(key);
            if (job) {
                releaseFileIds(job->visited);
                Server::instance()->jobScheduler()->abort(job);
            }
            removeDependencies(fileId);
            ++count;
            unlink(sourceFilePath(fileId).constData());
        } else {
            ++it;
        }
    }
    return count;
}

int Project::startDirtyJobs(Dirty *dirty, const UnsavedFiles &unsavedFiles, const std::shared_ptr<Connection> &wait)
{
    const JobScheduler::JobScope scope(Server::instance()->jobScheduler());
    List<Source> toIndex;
    for (const auto &source : mSources) {
        if (source.second.flags & Source::Active && dirty->isDirty(source.second)) {
            toIndex << source.second;
        }
    }
    const Set<uint32_t> dirtyFiles = dirty->dirtied();

    {
        std::lock_guard<std::mutex> lock(mMutex);
        for (const auto &fileId : dirtyFiles) {
            mVisitedFiles.remove(fileId);
        }
    }

    std::weak_ptr<Connection> weakConn(wait);
    for (const auto &source : toIndex) {
        std::shared_ptr<IndexerJob> job(new IndexerJob(source, IndexerJob::Dirty, shared_from_this(), unsavedFiles));
        if (wait) {
            job->destroyed.connect([weakConn](IndexerJob *) {
                    if (auto strong = weakConn.lock()) {
                        strong->finish();
                    }
                });
        }
        index(job);
    }

    return toIndex.size();
}

bool Project::isIndexed(uint32_t fileId) const
{
    {
        std::lock_guard<std::mutex> lock(mMutex);
        if (mVisitedFiles.contains(fileId))
            return true;
    }

    const uint64_t key = Source::key(fileId, 0);
    auto it = mSources.lower_bound(key);
    if (it != mSources.end()) {
        uint32_t f, b;
        Source::decodeKey(it->first, f, b);
        if (f == fileId)
            return true;
    }
    return false;
}

const Set<uint32_t> &Project::suspendedFiles() const
{
    return mSuspendedFiles;
}

void Project::clearSuspendedFiles()
{
    mSuspendedFiles.clear();
}

bool Project::toggleSuspendFile(uint32_t file)
{
    if (!mSuspendedFiles.insert(file)) {
        mSuspendedFiles.remove(file);
        return false;
    }
    return true;
}

bool Project::isSuspended(uint32_t file) const
{
    return mSuspendedFiles.contains(file);
}

void Project::updateFixIts(const Set<uint32_t> &visited, FixIts &fixIts)
{
    for (auto v : visited) {
        const auto fit = fixIts.find(v);
        if (fit == fixIts.end()) {
            mFixIts.erase(v);
        } else {
            mFixIts[v] = fit->second;
        }
    }
}

String Project::fixIts(uint32_t fileId) const
{
    const auto it = mFixIts.find(fileId);
    String out;
    if (it != mFixIts.end()) {
        const Set<FixIt> &fixIts = it->second;
        if (!fixIts.isEmpty()) {
            auto f = fixIts.end();
            do {
                --f;
                if (!out.isEmpty())
                    out.append('\n');
                out.append(String::format<32>("%d:%d %d %s", f->line, f->column, f->length, f->text.constData()));

            } while (f != fixIts.begin());
        }
    }
    return out;
}

void Project::findSymbols(const String &string,
                          const std::function<void(SymbolMatchType, const String &, const Set<Location> &)> &inserter,
                          Flags<QueryMessage::Flag> queryFlags,
                          uint32_t fileFilter)
{
    const bool wildcard = queryFlags & QueryMessage::WildcardSymbolNames && (string.contains('*') || string.contains('?'));
    const bool caseInsensitive = queryFlags & QueryMessage::MatchCaseInsensitive;
    const String::CaseSensitivity cs = caseInsensitive ? String::CaseInsensitive : String::CaseSensitive;
    String lowerBound;
    if (wildcard) {
        if (!caseInsensitive) {
            for (int i=0; i<string.size(); ++i) {
                if (string.at(i) == '?' || string.at(i) == '*') {
                    lowerBound = string.left(i);
                    break;
                }
            }
        }
    } else if (!caseInsensitive) {
        lowerBound = string;
    }

    auto processFile = [this, &lowerBound, &string, wildcard, cs, inserter](uint32_t file) {
        auto symNames = openSymbolNames(file);
        if (!symNames)
            return;
        const int count = symNames->count();
        // error() << "Looking at" << count << Location::path(dep.first)
        //         << lowerBound << string;
        int idx = 0;
        if (!lowerBound.isEmpty()) {
            idx = symNames->lowerBound(lowerBound);
            if (idx == -1) {
                return;
            }
        }

        for (int i=idx; i<count; ++i) {
            const String &entry = symNames->keyAt(i);
            // error() << i << count << entry;
            SymbolMatchType type = Exact;
            if (!string.isEmpty()) {
                if (wildcard) {
                    if (!Rct::wildCmp(string.constData(), entry.constData(), cs)) {
                        continue;
                    }
                    type = Wildcard;
                } else if (!entry.startsWith(string, cs)) {
                    if (cs == String::CaseInsensitive) {
                        continue;
                    } else {
                        break;
                    }
                    type = StartsWith;
                } else if (entry.size() != string.size()) {
                    type = StartsWith;
                }
            }
            inserter(type, entry, symNames->valueAt(i));
        }
    };

    if (fileFilter) {
        processFile(fileFilter);
    } else {
        for (const auto &dep : mDependencies) {
            processFile(dep.first);
        }
    }
}

List<RTags::SortedSymbol> Project::sort(const Set<Symbol> &symbols, Flags<QueryMessage::Flag> flags)
{
    List<RTags::SortedSymbol> sorted;
    sorted.reserve(symbols.size());
    for (const Symbol &symbol : symbols) {
        RTags::SortedSymbol node(symbol.location);
        if (!symbol.isNull()) {
            node.isDefinition = symbol.isDefinition();
            if (flags & QueryMessage::DeclarationOnly && node.isDefinition) {
                const Symbol decl = findTarget(symbol);
                if (!decl.isNull() && !decl.isDefinition()) {
                    assert(decl.usr == symbol.usr);
                    continue;
                }
            } else if (flags & QueryMessage::DefinitionOnly && !node.isDefinition) {
                continue;
            }
            node.kind = symbol.kind;
        }
        sorted.push_back(node);
    }

    if (flags & QueryMessage::ReverseSort) {
        std::sort(sorted.begin(), sorted.end(), std::greater<RTags::SortedSymbol>());
    } else {
        std::sort(sorted.begin(), sorted.end());
    }
    return sorted;
}

void Project::watch(const Path &dir, WatchMode mode)
{
    if (!dir.isEmpty()) {
        const auto it = mWatchedPaths.find(dir);
        if (it != mWatchedPaths.end()) {
            it->second |= mode;
            return;
        }
        const Path resolved = dir.resolved();
        if ((Server::instance()->options().options & Server::WatchSystemPaths) || !resolved.isSystem()) {
            auto &m = mWatchedPaths[resolved];
            if (!m) {
                mWatcher.watch(resolved);
            }
            m |= mode;
        }
    }
}

void Project::watchFile(uint32_t fileId)
{
    watch(Location::path(fileId).parentDir(), Watch_SourceFile);
}

void Project::clearWatch(Flags<WatchMode> mode)
{
    auto it = mWatchedPaths.begin();
    while (it != mWatchedPaths.end()) {
        it->second &= ~mode;
        if (!it->second) {
            mWatcher.unwatch(it->first);
            mWatchedPaths.erase(it++);
        } else {
            ++it;
        }
    }
}

void Project::unwatch(const Path &dir, WatchMode mode)
{
    auto it = mWatchedPaths.find(dir);
    if (it == mWatchedPaths.end()) {
        mWatchedPaths.find(dir.resolved());
        if (it == mWatchedPaths.end()) {
            error() << "We're not watching this directory" << dir;
            return;
        }
    }
    if (!(it->second &= ~mode)) {
        mWatcher.unwatch(it->first);
        mWatchedPaths.erase(it);
    }
}

String Project::toCompilationDatabase() const
{
    const Flags<Source::CommandLineFlag> flags = (Source::IncludeCompiler
                                                  | Source::IncludeSourceFile
                                                  | Source::IncludeDefines
                                                  | Source::IncludeIncludepaths
                                                  | Source::QuoteDefines
                                                  | Source::FilterBlacklist);
    Value ret(List<Value>(mSources.size()));
    int i = 0;
    for (const auto &source : mSources) {
        Value unit;
        unit["directory"] = source.second.directory;
        unit["file"] = source.second.sourceFile();
        unit["command"] = String::join(source.second.toCommandLine(flags), " ").constData();
        ret[i++] = unit;
    }

    return ret.toJSON(true);
}

Symbol Project::findSymbol(const Location &location, int *index)
{
    if (index)
        *index = -1;
    if (location.isNull())
        return Symbol();
    auto symbols = openSymbols(location.fileId());
    if (!symbols || !symbols->count())
        return Symbol();

    bool exact = false;
    int idx = symbols->lowerBound(location, &exact);
    if (exact) {
        if (index)
            *index = idx;
        return symbols->valueAt(idx);
    }
    switch (idx) {
    case 0:
        return Symbol();
    case -1:
        idx = symbols->count() - 1;
        break;
    default:
        --idx;
        break;
    }

    const Symbol &ret = symbols->valueAt(idx);
    if (ret.location.fileId() != location.fileId()
        || ret.location.line() != location.line()
        || (location.column() - ret.location.column() >= ret.symbolLength)) {
        return Symbol();
    }
    if (index)
        *index = idx;
    return ret;
}

Set<Symbol> Project::findTargets(const Symbol &symbol)
{
    Set<Symbol> ret;
    if (symbol.isNull())
        return ret;
    if (symbol.isClass() && symbol.isDefinition())
        return ret;

    auto sameKind = [&symbol](CXCursorKind kind) {
        if (kind == symbol.kind)
            return true;
        if (symbol.isClass())
            return kind == CXCursor_ClassDecl || kind == CXCursor_StructDecl;
        return false;
    };

    switch (symbol.kind) {
    case CXCursor_ClassDecl:
    case CXCursor_ClassTemplate:
    case CXCursor_StructDecl:
    case CXCursor_FunctionDecl:
    case CXCursor_CXXMethod:
    case CXCursor_Destructor:
    case CXCursor_Constructor:
    case CXCursor_FieldDecl:
    case CXCursor_VarDecl:
    case CXCursor_FunctionTemplate: {
        const Set<Symbol> symbols = findByUsr(symbol.usr, symbol.location.fileId(), symbol.isDefinition() ? ArgDependsOn : DependsOnArg);
        for (const auto &c : symbols) {
            if (sameKind(c.kind) && symbol.isDefinition() != c.isDefinition()) {
                ret.insert(c);
                break;
            }
        }
        if (!ret.isEmpty())
            break; }
        // fall through
    default:
        for (const String &usr : findTargetUsrs(symbol.location)) {
            ret.unite(findByUsr(usr, symbol.location.fileId(), Project::ArgDependsOn));
        }
        break;
    }

    return ret;
}

Set<Symbol> Project::findByUsr(const String &usr, uint32_t fileId, DependencyMode mode)
{
    assert(fileId);
    Set<Symbol> ret;
    if (usr.startsWith("/")) {
        Symbol sym;
        sym.location = Location(Location::fileId(usr), 1, 1);
        ret.insert(sym);
        return ret;
    }
    if (mDeclarations.contains(usr)) {
        assert(!mDeclarations.value(usr).isEmpty());
        for (const auto &dep : mDependencies) {
            auto usrs = openUsrs(dep.first);
            if (usrs) {
                for (const Location &loc : usrs->value(usr)) {
                    const Symbol c = findSymbol(loc);
                    if (!c.isNull())
                        ret.insert(c);
                }
            }
        }
    } else {
        for (uint32_t file : dependencies(fileId, mode)) {
            auto usrs = openUsrs(file);
            // error() << usrs << Location::path(file) << usr;
            if (usrs) {
                for (const Location &loc : usrs->value(usr)) {
                    // error() << "got a loc" << loc;
                    const Symbol c = findSymbol(loc);
                    if (!c.isNull())
                        ret.insert(c);
                }
                // for (int i=0; i<usrs->count(); ++i) {
                //     error() << i << usrs->count() << usrs->keyAt(i) << usrs->valueAt(i);
                // }
            }
        }
    }
    return ret;
}

static Set<Symbol> findReferences(const Set<Symbol> &inputs,
                                  const std::shared_ptr<Project> &project,
                                  std::function<bool(const Symbol &, const Symbol &)> filter)
{
    Set<Symbol> ret;
    // const bool isClazz = s.isClass();
    for (const Symbol &input : inputs) {
        //warning() << "Calling findReferences" << input.location;
        auto process = [&](uint32_t dep) {
            // error() << "Looking at file" << Location::path(dep) << "for input" << input.location;
            auto targets = project->openTargets(dep);
            if (targets) {
                const Set<Location> locations = targets->value(input.usr);
                // error() << "Got locations for usr" << input.usr << locations;
                for (const auto &loc : locations) {
                    auto sym = project->findSymbol(loc);
                    if (filter(input, sym))
                        ret.insert(sym);
                }
            }
        };

        if (project->isDeclaration(input.usr)) {
            for (auto dep : project->dependencies())
                process(dep.first);
        } else {
            for (auto dep : project->dependencies(input.location.fileId(), Project::DependsOnArg))
                process(dep);
        }

    }
    return ret;
}

static Set<Symbol> findReferences(const Symbol &in,
                                  const std::shared_ptr<Project> &project,
                                  std::function<bool(const Symbol &, const Symbol &)> filter,
                                  Set<Symbol> *inputsPtr = 0)
{
    Set<Symbol> inputs;
    Symbol s;
    if (in.isReference()) {
        s = project->findTarget(in);
    } else {
        s = in;
    }

    // error() << "findReferences" << s.location;
    switch (s.kind) {
    case CXCursor_CXXMethod:
        if (s.flags & Symbol::VirtualMethod) {
            inputs = project->findVirtuals(s);
            break;
        }
        // fall through
    case CXCursor_FunctionTemplate:
    case CXCursor_FunctionDecl:
    case CXCursor_ClassTemplate:
    case CXCursor_ClassDecl:
    case CXCursor_StructDecl:
    case CXCursor_UnionDecl:
    case CXCursor_TypedefDecl:
    case CXCursor_Namespace:
    case CXCursor_Constructor:
    case CXCursor_Destructor:
    case CXCursor_ConversionFunction:
    case CXCursor_NamespaceAlias:
        inputs = project->findByUsr(s.usr, s.location.fileId(), s.isDefinition() ? Project::ArgDependsOn : Project::DependsOnArg);
        break;
    default:
        inputs.insert(s);
        break;
    case CXCursor_FirstInvalid:
        return Set<Symbol>();
    }
    if (inputsPtr)
        *inputsPtr = inputs;
    return findReferences(inputs, project, filter);
}

Set<Symbol> Project::findCallers(const Symbol &symbol)
{
    const bool isClazz = symbol.isClass();
    return ::findReferences(symbol, shared_from_this(), [isClazz](const Symbol &input, const Symbol &ref) {
            if (isClazz && ref.isConstructorOrDestructor())
                return false;
            if (ref.isReference()
                || (input.kind == CXCursor_Constructor && (ref.kind == CXCursor_VarDecl || ref.kind == CXCursor_FieldDecl))) {
                return true;
            }
            return false;
        });
}

Set<Symbol> Project::findAllReferences(const Symbol &symbol)
{
    if (symbol.isNull())
        return Set<Symbol>();

    Set<Symbol> inputs;
    inputs.insert(symbol);
    inputs.unite(findByUsr(symbol.usr, symbol.location.fileId(), DependsOnArg));
    Set<Symbol> ret = inputs;
    for (const auto &input : inputs) {
        Set<Symbol> inputLocations;
        ret.unite(::findReferences(input, shared_from_this(), [](const Symbol &, const Symbol &) {
                    return true;
                }, &inputLocations));
        ret.unite(inputLocations);
    }
    return ret;
}

Set<Symbol> Project::findVirtuals(const Symbol &symbol)
{
    if (symbol.kind != CXCursor_CXXMethod || !(symbol.flags & Symbol::VirtualMethod))
        return Set<Symbol>();

    Symbol parent = [this](const Symbol &symbol) {
        for (const String &usr : findTargetUsrs(symbol.location)) {
            const Set<Symbol> syms = findByUsr(usr, symbol.location.fileId(), ArgDependsOn);
            for (const Symbol &sym : syms) {
                if (findTargetUsrs(sym.location).isEmpty()) {
                    return sym;
                }
            }
        }
        return symbol;
    }(symbol);

    assert(!parent.isNull());
    if (parent.isDefinition()) {
        const auto target = findTarget(parent);
        if (!target.isNull()) {
            parent = target;
        }
    }

    Set<Symbol> symSet;
    symSet.insert(parent);
    Set<Symbol> ret = ::findReferences(symSet, shared_from_this(), [](const Symbol &, const Symbol &ref) {
            // error() << "considering" << ref.location << ref.kindSpelling();
            if (ref.kind == CXCursor_CXXMethod) {
                return true;
            }
            return false;
        });
    ret.insert(parent);
    const Symbol target = findTarget(parent);
    if (!target.isNull())
        ret.insert(target);
    return ret;
}

Set<String> Project::findTargetUsrs(const Location &loc)
{
    Set<String> usrs;
    auto targets = openTargets(loc.fileId());
    if (targets) {
        const int count = targets->count();
        for (int i=0; i<count; ++i) {
            if (targets->valueAt(i).contains(loc))
                usrs.insert(targets->keyAt(i));
        }
    }
    return usrs;
}

Set<Symbol> Project::findSubclasses(const Symbol &symbol)
{
    assert(symbol.isClass() && symbol.isDefinition());
    Set<Symbol> ret;
    for (uint32_t dep : dependencies(symbol.location.fileId(), DependsOnArg)) {
        auto symbols = openSymbols(dep);
        if (symbols) {
            const int count = symbols->count();
            for (int i=0; i<count; ++i) {
                const Symbol s = symbols->valueAt(i);
                if (s.baseClasses.contains(symbol.usr))
                    ret.insert(s);
            }
        }
    }
    return ret;
}

void Project::beginScope()
{
    assert(!mFileMapScope);
    mFileMapScope.reset(new FileMapScope(shared_from_this(), Server::instance()->options().maxFileMapScopeCacheSize));
}

void Project::endScope()
{
    assert(mFileMapScope);
    mFileMapScope.reset();
}

String Project::dumpDependencies(uint32_t fileId, const List<String> &args) const
{
    String ret;

    DependencyNode *node = mDependencies.value(fileId);
    if (!node)
        return String::format<128>("Can't find node for %s", Location::path(fileId).constData());

    if (!node->includes.isEmpty() && (args.isEmpty() || args.contains("includes"))) {
        if (args.size() != 1)
            ret += String::format<256>("  %s includes:\n", Location::path(fileId).constData());
        for (const auto &include : node->includes) {
            ret += String::format<256>("    %s\n", Location::path(include.first).constData());
        }
    }
    if (!node->dependents.isEmpty() && (args.isEmpty() || args.contains("included-by"))) {
        if (args.size() != 1)
            ret += String::format<256>("  %s is included by:\n", Location::path(fileId).constData());
        for (const auto &include : node->dependents) {
            ret += String::format<256>("    %s\n", Location::path(include.first).constData());
        }
    }

    if (args.isEmpty() || args.contains("depends-on")) {
        bool first = args.size() != 1;
        for (auto dep : dependencies(fileId, Project::ArgDependsOn)) {
            if (dep == fileId)
                continue;
            if (first) {
                first = false;
                ret += String::format<256>("  %s depends on:\n", Location::path(fileId).constData());
            }
            ret += String::format<256>("    %s\n", Location::path(dep).constData());
        }
    }

    if (args.isEmpty() || args.contains("depended-on")) {
        bool first = args.size() != 1;
        for (auto dep : dependencies(fileId, Project::DependsOnArg)) {
            if (dep == fileId)
                continue;
            if (first) {
                first = false;
                ret += String::format<256>("  %s is depended on by:\n", Location::path(fileId).constData());
            }
            ret += String::format<256>("    %s\n", Location::path(dep).constData());
        }
    }

    if (args.isEmpty() || args.contains("tree-depends-on")) {
        Set<DependencyNode*> seen;
        if (args.size() != 1) {
            ret += String::format<256>("  %s include tree:\n", Location::path(fileId).constData());
        }

        int depth = 1;
        auto process = [&](DependencyNode *n) {
            process(n);
        };

        while (!nodes.isEmpty()) {
            std::pair<DependencyNode *, int> n = nodes.takeLast();
            assert(n.first);
            ret += String::format<256>("%s%s", String(n.second * 2, ' ').constData(), Location::path(n.first->fileId).constData());
            if (seen.insert(n.first) && !n.first->includes.isEmpty()) {
                ret += " includes:\n";
                for (const auto &pair : n.first->includes) {
                    nodes.append(std::make_pair(pair.second, n.second + 1));
                }
            } else {
                ret += '\n';
            }
        }
    }

    return ret;
}

void Project::dirty(uint32_t fileId)
{
    SimpleDirty dirty;
    Set<uint32_t> dirtyFiles;
    dirtyFiles.insert(fileId);
    dirty.init(dirtyFiles, shared_from_this());
    startDirtyJobs(&dirty);
}

bool Project::validate(uint32_t fileId, String *err) const
{
    Path path;
    String error;
    {
        path = sourceFilePath(fileId, fileMapName(SymbolNames));
        FileMap<String, Set<Location> > fileMap;
        if (!fileMap.load(path, &error))
            goto error;
    }
    {
        path = sourceFilePath(fileId, fileMapName(Symbols));
        FileMap<Location, Symbol> fileMap;
        if (!fileMap.load(path, &error))
            goto error;
    }
    {
        path = sourceFilePath(fileId, fileMapName(Targets));
        FileMap<String, Set<Location> > fileMap;
        if (!fileMap.load(path, &error))
            goto error;
    }
    {
        path = sourceFilePath(fileId, fileMapName(Usrs));
        FileMap<String, Set<Location> > fileMap;
        if (!fileMap.load(path, &error))
            goto error;
    }
    return true;
error:
    if (err)
        Log(err) << "Error during validation:" << Location::path(fileId) << error << path;
    return false;
}

void Project::loadFailed(uint32_t fileId)
{
    const Path sourcePath = Location::path(fileId);
    if (sourcePath.isSource()) {
        if (Server::instance()->jobScheduler()->increasePriority(fileId))
            return;
    } else { // header
        for (auto dep : dependencies(fileId, Project::DependsOnArg)) {
            if (Location::path(dep).isSource()) {
                auto src = mSources.lower_bound(Source::key(dep, 0));
                if (src != mSources.end()) {
                    uint32_t f, b;
                    Source::decodeKey(src->first, f, b);
                    if (f == dep && Server::instance()->jobScheduler()->increasePriority(dep)) {
                        return;
                    }
                }
            }
        }
    }

    dirty(fileId); // file might have gone missing
}

template <typename T>
static inline String toString(const T &t, int &max)
{
    String ret;
    Log(&ret, LogOutput::NoTypename) << t;
    max = std::max(max, ret.size());
    ret.replace('\n', ' ');
    return ret;
}

template <>
inline String toString(const String &str, int &max)
{
    max = std::max(max, str.size());
    String ret = str;
    ret.replace('\n', ' ');
    return ret;
}

static inline void fixString(String &string, int size)
{
    if (string.size() != size)
        string.append(String(size - string.size(), ' '));
}

static List<String> split(const String &value, int max)
{
    List<String> words = value.split(' ', String::KeepSeparators);
    List<String> ret(1);
    int i = 0;
    while (i < words.size()) {
        const String &word = words.at(i);
        if (ret.last().size() && ret.last().size() + word.size() > max) {
            fixString(ret.last(), max);
            ret.append(String());
            continue;
        }

        if (word.size() > max) {
            assert(ret.last().isEmpty());
            for (int j=0; j<word.size(); j += max) {
                if (j)
                    ret.append(String());
                ret.last() = word.mid(j, max);
                fixString(ret.last(), max);
            }
        } else {
            ret.last().append(word);
        }
        ++i;
    }

    fixString(ret.last(), max);
    return ret;
}

template <typename T> struct PreferComma { enum { value = 0 }; };
template <typename T> struct PreferComma<Set<T> > { enum { value = 1 }; };

template <typename T>
static List<String> formatField(const String &value, int max)
{
    List<String> ret;
    if (value.size() <= max) {
        ret << value.padded(String::End, max);
    } else {
        if (PreferComma<T>::value)
            ret = value.split(',', String::KeepSeparators);

        if (ret.size() > 1) {
            for (int i=0; i<ret.size(); ++i) {
                if (ret.at(i).size() > max) {
                    auto split = ::split(ret.at(i), max);
                    ret.remove(i, 1);
                    ret.insert(i, split);
                    i += split.size() - 1;
                } else if (ret.at(i).size() != max) {
                    ret[i].append(String(max - ret.at(i).size(), ' '));
                }
            }
        } else {
            ret = split(value, max);
        }
    }
    return ret;
}
template <typename Key, typename Value>
static String formatTable(const String &name, const std::shared_ptr<FileMap<Key, Value> > &fileMap, int width)
{
    width -= 7; // padding
    List<String> keys, values;
    const int count = fileMap->count();
    int maxKey = 0;
    int maxValue = 0;
    for (int i=0; i<count; ++i) {
        keys << toString(fileMap->keyAt(i), maxKey);
        values << toString(fileMap->valueAt(i), maxValue);
    }
    if (maxKey + maxValue > width) {
        if (maxKey < maxValue) {
            maxKey = std::min(maxKey, static_cast<int>(width * .4));
            maxValue = std::min(maxValue, width - maxKey);
        } else {
            maxValue = std::min(maxValue, static_cast<int>(width * .4));
            maxKey = std::min(maxKey, width - maxValue);

        }
    }

    String ret;
    ret.reserve((count + 3) * (maxKey + maxValue + 7));
    ret << name << '\n' << String(name.size(), '-') << '\n';
    const String keyFill(maxKey, ' ');
    const String valueFill(maxValue, ' ');
    for (int i=0; i<count; ++i) {
        const List<String> key = formatField<Key>(keys.at(i), maxKey);
        const List<String> value = formatField<Value>(values.at(i), maxValue);
        const int c = std::max(key.size(), value.size());
        char ch = '|';
        for (int j=0; j<c; ++j) {
            ret << ch << ' ' << key.value(j, keyFill) << ' ' << ch << ' ' << value.value(j, valueFill) << ' ' << ch << '\n';
            ch = '*';
        }
    }
    return ret;
}

void Project::dumpFileMaps(const std::shared_ptr<QueryMessage> &msg, const std::shared_ptr<Connection> &conn)
{
    beginScope();
    String err;

    Path path;
    List<String> args;
    Deserializer deserializer(msg->query());
    deserializer >> path >> args;
    const uint32_t fileId = Location::fileId(path);
    assert(fileId);
    assert(isIndexed(fileId));

    if (args.empty() || args.contains("symbols")) {
        if (auto tbl = openSymbols(fileId, &err)) {
            conn->write(formatTable("Symbols:", tbl, msg->terminalWidth()));
        } else {
            conn->write(err);
        }
    }

    if (args.empty() || args.contains("symbolnames")) {
        if (auto tbl = openSymbolNames(fileId, &err)) {
            conn->write(formatTable("Symbol names:", tbl, msg->terminalWidth()));
        } else {
            conn->write(err);
        }
    }

    if (args.empty() || args.contains("targets")) {
        if (auto tbl = openTargets(fileId, &err)) {
            conn->write(formatTable("Targets:", tbl, msg->terminalWidth()));
        } else {
            conn->write(err);
        }
    }

    if (args.empty() || args.contains("usrs")) {
        if (auto tbl = openUsrs(fileId, &err)) {
            conn->write(formatTable("Usrs:", tbl, msg->terminalWidth()));
        } else {
            conn->write(err);
        }
    }

    endScope();
}

void Project::prepare(uint32_t fileId)
{
    if (fileId && isIndexed(fileId)) {
        beginScope();
        String err;
        openSymbolNames(fileId, &err);
        openSymbols(fileId, &err);
        openTargets(fileId, &err);
        openUsrs(fileId, &err);
        debug() << "Prepared" << Location::path(fileId);
        endScope();
    }
}

template <typename T>
size_t estimateMemory(const T &t);
size_t estimateMemory(const String &string);
size_t estimateMemory(const Source::Define &define);
size_t estimateMemory(const Source::Include &include);
size_t estimateMemory(const Source &source);
template <typename T>
size_t estimateMemory(const std::shared_ptr<T> &ptr);
template <typename T>
size_t estimateMemory(const T *t);
template <typename T>
size_t estimateMemory(const Set<T> &container);
template <typename T>
size_t estimateMemory(const List<T> &container);
template <typename Key, typename Value>
size_t estimateMemory(const Map<Key, Value> &container);
template <typename Key, typename Value>
size_t estimateMemory(const Hash<Key, Value> &container);

// impl
template <typename T>
size_t estimateMemory(const T &t)
{
    return sizeof(t);
}

size_t estimateMemory(const String &string)
{
    return string.size() + 1 + sizeof(string) + (sizeof(size_t) + sizeof(size_t));
}

size_t estimateMemory(const Source::Define &define)
{
    return estimateMemory(define.define) + estimateMemory(define.value);
}

size_t estimateMemory(const Source::Include &include)
{
    return estimateMemory(include.type) + estimateMemory(include.path);
}

size_t estimateMemory(const Source &source)
{
    size_t ret = sizeof(Source);
    ret += estimateMemory(source.extraCompiler);
    ret += estimateMemory(source.defines);
    ret += estimateMemory(source.includePaths);
    ret += estimateMemory(source.directory);
    return ret;
}

template <typename T>
size_t estimateMemory(const std::shared_ptr<T> &ptr)
{
    size_t ret = sizeof(ptr) + sizeof(size_t);
    if (ptr) {
        ret += estimateMemory(*ptr);
    } else {
        ret += sizeof(T);
    }
    return ret;
}

template <typename T>
size_t estimateMemory(const T *t)
{
    if (t)
        return estimateMemory(*t);
    return 0;
}

template <typename T>
size_t estimateMemory(const Set<T> &container)
{
    size_t ret = sizeof(Set<T>) + sizeof(void*);
    for (const T &value : container) {
        ret += estimateMemory(value) + (sizeof(void*) * 2); // might not be entirely fair to std::vector
    }
    return ret;
}

template <typename T>
size_t estimateMemory(const List<T> &container)
{
    size_t ret = sizeof(List<T>) + sizeof(void*);
    for (const T &value : container) {
        ret += estimateMemory(value);
    }
    return ret;
}

template <typename T>
size_t estimateKeyValueContainer(const T &t)
{
    size_t ret = sizeof(T) + sizeof(void*);
    for (const auto &pair : t) {
        ret += estimateMemory(pair.first) + estimateMemory(pair.second) + (sizeof(void*) * 2);
    }
    return ret;
}

template <typename Key, typename Value>
size_t estimateMemory(const Map<Key, Value> &container)
{
    return estimateKeyValueContainer(container);
}

template <typename Key, typename Value>
size_t estimateMemory(const Hash<Key, Value> &container)
{
    return estimateKeyValueContainer(container);
}

String Project::estimateMemory() const
{
    List<String> ret;
    size_t total = 0;
    auto add = [&ret, &total](const char *name, size_t size) {
        total += size;
        ret << String::format<128>("%s: %.2fmb", name, size / (1024.0 * 1024.0));
    };
    add("Paths", ::estimateMemory(mFiles));
    add("Visited files", ::estimateMemory(mVisitedFiles));
    add("Diagnostics", ::estimateMemory(mDiagnostics));
    add("Active jobs", ::estimateMemory(mActiveJobs));
    add("Fixits", ::estimateMemory(mFixIts));
    add("Pending dirty files", ::estimateMemory(mPendingDirtyFiles));
    add("Declarations", ::estimateMemory(mDeclarations));
    add("Sources", ::estimateMemory(mSources));
    add("Suspended files", ::estimateMemory(mSuspendedFiles));
    size_t deps = ::estimateMemory(mDependencies);
    for (const auto &dep : mDependencies) {
        deps += ::estimateMemory(*dep.second);
    }
    add("Dependencies", deps);
    add("Total", total);
    return String::join(ret, "\n");
}
