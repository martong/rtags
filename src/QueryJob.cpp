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

#include "QueryJob.h"
#include "RTags.h"
#include <rct/EventLoop.h>
#include "Server.h"
#include <regex>
#include "QueryMessage.h"
#include "Project.h"

QueryJob::QueryJob(const std::shared_ptr<QueryMessage> &query,
                   const std::shared_ptr<Project> &proj,
                   Flags<JobFlag> jobFlags)
    : mAborted(false), mLinesWritten(0), mQueryMessage(query), mJobFlags(jobFlags), mProject(proj), mFileFilter(0)
{
    if (mProject)
        mProject->beginScope();
    assert(query);
    if (query->flags() & QueryMessage::SilentQuery)
        setJobFlag(QuietJob);
    const List<QueryMessage::PathFilter> &pathFilters = query->pathFilters();
    if (!pathFilters.isEmpty()) {
        if (pathFilters.size() == 1 && pathFilters.first().mode == QueryMessage::PathFilter::Self) {
            mFileFilter = Location::fileId(pathFilters.first().pattern);
        }
        if (!mFileFilter) {
            for (const QueryMessage::PathFilter &filter : pathFilters) {
                if (filter.mode == QueryMessage::PathFilter::Dependency) {
                    uint32_t f = Location::fileId(filter.pattern);
                    if (f && mProject)
                        mFilters.append(std::shared_ptr<Filter>(new DependencyFilter(f, mProject)));
                } else if (query->flags() & QueryMessage::MatchRegex) {
                    mFilters.append(std::shared_ptr<Filter>(new RegexFilter(filter.pattern)));
                } else {
                    mFilters.append(std::shared_ptr<Filter>(new PathFilter(filter.pattern)));
                }
            }
        }
    }
    mKindFilters = query->kindFilters();
}

QueryJob::~QueryJob()
{
    if (mProject)
        mProject->endScope();
}

bool QueryJob::write(const String &out, Flags<WriteFlag> flags)
{
    if ((mJobFlags & WriteUnfiltered) || (flags & Unfiltered) || filter(out)) {
        if ((mJobFlags & QuoteOutput) && !(flags & DontQuote)) {
            String o((out.size() * 2) + 2, '"');
            char *ch = o.data() + 1;
            int l = 2;
            for (int i=0; i<out.size(); ++i) {
                const char c = out.at(i);
                if (c == '"') {
                    *ch = '\\';
                    ch += 2;
                    l += 2;
                } else {
                    ++l;
                    *ch++ = c;
                }
            }
            o.truncate(l);
            return writeRaw(o, flags);
        } else {
            return writeRaw(out, flags);
        }
    }
    return true;
}

bool QueryJob::writeRaw(const String &out, Flags<WriteFlag> flags)
{
    assert(mConnection);
    if (!(flags & IgnoreMax) && mQueryMessage) {
        const int max = mQueryMessage->max();
        if (max != -1 && mLinesWritten == max) {
            return false;
        }
        assert(mLinesWritten < max || max == -1);
        ++mLinesWritten;
    }

    if (!(mJobFlags & QuietJob))
        error("=> %s", out.constData());

    if (mConnection) {
        if (!mConnection->write(out)) {
            abort();
            return false;
        }
        return true;
    }

    return true;
}

bool QueryJob::write(const Location &location, Flags<WriteFlag> flags)
{
    if (location.isNull())
        return false;
    if (!(flags & Unfiltered)) {
        if (!filterLocation(location))
            return false;
        flags |= Unfiltered;
    }
    String out = location.key(keyFlags());
    const bool containingFunction = queryFlags() & QueryMessage::ContainingFunction;
    const bool cursorKind = queryFlags() & QueryMessage::CursorKind;
    const bool displayName = queryFlags() & QueryMessage::DisplayName;
    if (containingFunction || cursorKind || displayName || !mKindFilters.isEmpty()) {
        int idx;
        Symbol symbol = project()->findSymbol(location, &idx);
        if (symbol.isNull()) {
            error() << "Somehow can't find" << location << "in symbols";
        } else {
            if (!filterKind(symbol.kind))
                return false;
            if (displayName)
                out += '\t' + symbol.displayName();
            if (cursorKind)
                out += '\t' + symbol.kindSpelling();
            if (containingFunction) {
                const uint32_t fileId = location.fileId();
                const unsigned int line = location.line();
                const unsigned int column = location.column();
                auto fileMap = project()->openSymbols(location.fileId());
                if (fileMap) {
                    while (idx > 0) {
                        symbol = fileMap->valueAt(--idx);
                        if (symbol.location.fileId() != fileId)
                            break;
                        if (symbol.isDefinition()
                            && RTags::isContainer(symbol.kind)
                            && comparePosition(line, column, symbol.startLine, symbol.startColumn) >= 0
                            && comparePosition(line, column, symbol.endLine, symbol.endColumn) <= 0) {
                            out += "\tfunction: " + symbol.symbolName;
                            break;
                        }
                    }
                }
            }
        }
    }
    return write(out, flags);
}

bool QueryJob::write(const Symbol &symbol,
                     Flags<Symbol::ToStringFlag> toStringFlags,
                     Flags<WriteFlag> writeFlags)
{
    if (symbol.isNull())
        return false;

    if (!filterLocation(symbol.location))
        return false;

    if (!filterKind(symbol.kind))
        return false;

    return write(symbol.toString(toStringFlags, keyFlags(), project()), writeFlags|Unfiltered);
}

bool QueryJob::filter(const String &value) const
{
    if (mFilters.isEmpty() && !(queryFlags() & QueryMessage::FilterSystemIncludes))
        return true;

    const char *val = value.constData();
    while (*val && isspace(*val))
        ++val;
    const char *space = strchr(val, ' ');
    Path path;
    uint32_t fileId = 0;
    if (space) {
        path.assign(val, space - val);
    } else {
        path = val;
    }

    if (!path.isFile())
        return true; // non-file things go unfiltered

    if (queryFlags() & QueryMessage::FilterSystemIncludes && Path::isSystem(val))
        return false;
    fileId = Location::fileId(path);

    if (mFilters.isEmpty())
        return true;

    for (const std::shared_ptr<Filter> &filter : mFilters) {
        if (filter->match(fileId, path))
            return true;
    }
    return false;
}

int QueryJob::run(const std::shared_ptr<Connection> &connection)
{
    assert(connection);
    mConnection = connection;
    const int ret = execute();
    mConnection = 0;
    return ret;
}

bool QueryJob::filterLocation(const Location &loc) const
{
    if (mFileFilter && loc.fileId() != mFileFilter)
        return false;
    const int minLine = mQueryMessage ? mQueryMessage->minLine() : -1;
    if (minLine != -1) {
        assert(mQueryMessage);
        assert(mQueryMessage->maxLine() != -1);
        const int maxLine = mQueryMessage->maxLine();
        assert(maxLine != -1);
        const int line = loc.line();
        if (line < minLine || line > maxLine) {
            return false;
        }
    }
    if (!mFilters.isEmpty()) {
        for (const std::shared_ptr<Filter> &filter : mFilters) {
            if (filter->match(loc.fileId(), loc.path()))
                return true;
        }
        return false;
    }
    return true;
}

bool QueryJob::filterKind(CXCursorKind kind) const
{
    if (mKindFilters.isEmpty())
        return true;
    const String kindSpelling = Symbol::kindSpelling(kind);
    for (auto k : mKindFilters) {
        if (kindSpelling.contains(k, String::CaseInsensitive))
            return true;
    }
    return false;
}
