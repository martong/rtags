#include <sstream>
#include <stdio.h>
#include <unistd.h>
#include <limits.h>
#include <RTags.h>
#include <QtCore>
#include <GccArguments.h>
#include <Source.h>
#include "Database.h"
#include "Mmap.h"

using namespace RTags;

static Location locationFromKey(const QByteArray &key)
{
    Location loc;
    unsigned *uints[] = { &loc.file, &loc.line, &loc.column };
    const QList<QByteArray> parts = key.split(':');
    for (int i=0; i<qMin(3, parts.size()); ++i) {
        *uints[i] = parts.at(i).toUInt();
    }
    return loc;
}

static inline void writeExpect(Database *db)
{
    Q_ASSERT(db);
    QFile f("expect.txt");
    char buf[512];
    const bool cwd = getcwd(buf, 512);
    Q_ASSERT(cwd);
    (void)cwd;
    QByteArray path = buf;
    if (!path.endsWith("/"))
        path.append("/");
    if (!f.open(QIODevice::WriteOnly)) {
        fprintf(stderr, "Can't open expect.txt for writing\n");
        return;
    }
    Database::iterator *it = db->createIterator(Database::Targets);
    if (it->isValid()) {
        do {
            QByteArray s = it->key().constData();
            int colon = s.indexOf(':');
            {
                Path src = db->file(atoi(s.constData()));
                if (src.startsWith(path))
                    src.remove(0, path.size());
                s.replace(0, colon, src);
            }
            // src.append(it->key
            QByteArray target = db->locationToString(it->value<Location>());
            if (target.startsWith(path))
                target.remove(0, path.size());
            f.write("rc --no-context --paths-relative-to-root --follow-symbol ");
            f.write(s);
            f.write(" => ");
            f.write(target);
            f.putChar('\n');
            // qDebug() << s << target;
        } while (it->next());
    }
    delete it;
    it = db->createIterator(Database::References);
    if (it->isValid()) {
        do {
            if (it->key().endsWith(':')) {
                Location loc = db->createLocation(it->key());
                QSet<Location> refs = it->value<QSet<Location> >();
                qDebug() << db->locationToString(loc) << refs << it->key() << loc.file;
                continue;
            }
            // QByteArray s = it->key().constData();
            // if (s.endsWith(':')) {
            // int colon = s.indexOf(':');
            // {
            //     Path src = db->file(atoi(s.constData()));
            //     if (src.startsWith(path))
            //         src.remove(0, path.size());
            //     s.replace(0, colon, src);
            // }
            // // src.append(it->key
            // QByteArray target = db->locationToString(it->value<Location>());
            // if (target.startsWith(path))
            //     target.remove(0, path.size());
            // f.write("rc --no-context --paths-relative-to-root --follow-symbol ");
            // f.write(s);
            // f.write(" => ");
            // f.write(target);
            // f.putChar('\n');
            // // qDebug() << s << target;
        } while (it->next());
    }
    delete it;
    
    printf("Wrote expect.txt\n");
}

static inline void raw(Database *db, const QByteArray &file)
{
    const char *names[] = { "General: [", "Dictionary: [", "References: [", "Targets: [" };
    QFile f(file);
    if (!f.open(QIODevice::WriteOnly)) {
        fprintf(stderr, "Can't open %s for writing\n", file.constData());
        return;
    }
    QMap<QByteArray, QByteArray> entries[Database::NumConnectionTypes];
    for (int i=0; i<Database::NumConnectionTypes; ++i) {
        Database::iterator *it = db->createIterator(static_cast<Database::ConnectionType>(i));
        if (it->isValid()) {
            do {
                //Q_ASSERT_X(!entries[i].contains(it->key()), "raw()", (it->key() + " already exists in " + names[i]).constData());
                if (entries[i].contains(it->key()))
                    qDebug() << "already contains key" << it->key() << names[i];
                entries[i][it->key()] = it->value();
            } while (it->next());
        }
        delete it;
    }
    for (int i = 0; i < Database::NumConnectionTypes; ++i) {
        const QMap<QByteArray, QByteArray>& cat = entries[i];
        QMap<QByteArray, QByteArray>::const_iterator d = cat.begin();
        QMap<QByteArray, QByteArray>::const_iterator dend = cat.end();
        while (d != dend) {
            f.write(names[i]);
            f.write(d.key());
            f.write("] [");
            const QByteArray value = d.value();
            char buf[16];
            for (int i=0; i<value.size(); ++i) {
                char *b = buf;
                if (i > 0) {
                    buf[0] = ',';
                    buf[1] = ' ';
                    b = buf + 2;
                }
                const int ret = snprintf(b, 15, "0x%x", value.at(i));
                f.write(buf, ret + (b - buf));
            }
            f.write("]\n");
            ++d;
        }
    }
}

int main(int argc, char** argv)
{
    Mmap::init();

    int opt;
    char newLine = '\n';
    QByteArray rawFile;
    enum Mode {
        Normal,
        Expect,
        Raw
    } mode = Normal;
    while ((opt = getopt(argc, argv, "hnr:e")) != -1) {
        switch (opt) {
        case 'n':
            newLine = ' ';
            break;
        case 'r':
            mode = Raw;
            rawFile = optarg;
            break;
        case 'e':
            mode = Expect;
            break;
        case '?':
        case 'h':
        default:
            fprintf(stderr, "rdump -[rne]\n");
            return 0;
        }
    }

    QByteArray filename;
    if (optind >= argc) {
        filename = findRtagsDb();
    } else {
        filename = argv[optind];
    }

    Database* db = Database::create(filename, Database::ReadOnly);
    if (db->isOpened()) {
        switch (mode) {
        case Expect:
            writeExpect(db);
            return 0;
        case Raw:
            raw(db, rawFile);
            delete db;
            return 0;
        case Normal:
            break;
        }
        const char *names[] = { "General", "Dictionary", "References", "Targets" };
        for (int i=0; i<Database::NumConnectionTypes; ++i) {
            Database::iterator *it = db->createIterator(static_cast<Database::ConnectionType>(i));
            if (it->isValid()) {
                do {
                    const QByteArray key(it->key().constData(), it->key().size());
                    QByteArray coolKey;
                    if (i == Database::Targets || (i == Database::References && key.endsWith(":"))) {
                        coolKey = db->locationToString(locationFromKey(it->key())) + ' ';
                    }
                    printf("%s '%s' %s=> %d bytes", names[i], key.constData(), coolKey.constData(), it->value().size());
                    if (it->value().size() == 4) {
                        int t = it->value<int>();
                        printf(" (%d)%c", t, newLine);
                    } else {
                        switch (i) {
                        case Database::Targets:
                            printf(" (%s => %s)%c",
                                   db->locationToString(locationFromKey(key)).constData(),
                                   db->locationToString(it->value<Location>()).constData(), newLine);
                            break;
                        case Database::General:
                            if (key == "files") {
                                printf("%c", newLine);
                                foreach(const Path &file, it->value<QSet<Path> >()) {
                                    printf("    %s%c", file.constData(), newLine);
                                }
                            } else if (key == "filesByName") {
                                printf("%c", newLine);
                                const QHash<Path, int> filesToIndex = it->value<QHash<Path, int> >();
                                for (QHash<Path, int>::const_iterator it = filesToIndex.begin();
                                     it != filesToIndex.end(); ++it) {
                                    printf("    %s (id: %d)%c", it.key().constData(), it.value(), newLine);
                                }
                            } else if (key == "sourceDir") {
                                printf(" (%s)%c", it->value<Path>().constData(), newLine);
                            } else if (key == "sources") {
                                printf("%c", newLine);
                                foreach(const Source &src, it->value<QList<Source> >()) {
                                    printf("    %s (%s)", src.path.constData(),
                                           qPrintable(QDateTime::fromTime_t(src.lastModified).toString()));
                                    foreach(const QByteArray &arg, src.args) {
                                        printf(" %s", arg.constData());
                                    }
                                    printf("%cDependencies:%c", newLine, newLine);
                                    for (QHash<Path, quint64>::const_iterator it = src.dependencies.begin();
                                         it != src.dependencies.end(); ++it) {
                                        printf("      %s (%s)%c", it.key().constData(),
                                               qPrintable(QDateTime::fromTime_t(it.value()).toString()), newLine);
                                    }
                                }
                            } else {
                                fprintf(stderr, "Unknown key General: [%s]\n", key.constData());
                            }
                            break;
                        case Database::Dictionary: {
                            printf("%c", newLine);
                            const DictionaryHash &dh = it->value<DictionaryHash>();
                            for (DictionaryHash::const_iterator hit = dh.begin(); hit != dh.end(); ++hit) {
                                printf("    ");
                                const QList<QByteArray> &scope = hit.key();
                                for (int i=0; i<scope.size(); ++i) {
                                    printf("%s::", scope.at(i).constData());
                                }
                                printf("%s ", key.constData());
                                bool first = true;
                                foreach(const Location &l, hit.value()) {
                                    if (!first) {
                                        printf(", ");
                                    } else {
                                        first = false;
                                    }
                                    printf("%s", db->locationToString(l).constData());
                                }
                                printf("%c", newLine);
                            }
                            break; }
                        case Database::References:
                            printf("%c", newLine);
                            foreach(const Location &l, it->value<QSet<Location> >())
                                printf("    %s%c", db->locationToString(l).constData(), newLine);
                            break;
                        default:
                            printf("\n");
                            break;
                        }
                        if (newLine != '\n')
                            printf("\n");
                    }
                } while (it->next());
            }
            delete it;
        }
    }

    // if (createExpect)
    //     return writeExpect(filename) ? 0 : 2;
    // else
    //     dumpDatabase(filename, type);

    delete db;
    return 0;
}
