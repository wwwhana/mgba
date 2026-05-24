/* Copyright (c) 2013-2017 Jeffrey Pfau
 *
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */
#include "Window.h"

#include <QKeyEvent>
#include <QKeySequence>
#include <QCheckBox>
#include <QComboBox>
#include <QCoreApplication>
#include <QCryptographicHash>
#include <QDialog>
#include <QDialogButtonBox>
#include <QDir>
#include <QFile>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLabel>
#include <QLineEdit>
#include <QMenuBar>
#include <QMessageBox>
#include <QMimeData>
#include <QPainter>
#include <QProcess>
#include <QProcessEnvironment>
#include <QProgressDialog>
#include <QPushButton>
#include <QRadioButton>
#include <QRegularExpression>
#include <QScreen>
#include <QScrollArea>
#include <QStandardPaths>
#include <QVBoxLayout>
#include <QWidget>
#include <QWindow>

#ifdef Q_OS_WIN
#include <dwmapi.h>
#include <windows.h>
#endif

#ifdef USE_SQLITE3
#include "ArchiveInspector.h"
#include "library/LibraryController.h"
#endif

#include "AboutScreen.h"
#include "AudioProcessor.h"
#include "BattleChipView.h"
#include "CheatsView.h"
#include "ConfigController.h"
#include "CoreController.h"
#include "DebuggerConsole.h"
#include "DebuggerConsoleController.h"
#include "Display.h"
#include "DolphinConnector.h"
#include "CoreController.h"
#include "ForwarderView.h"
#include "FrameView.h"
#include "GBAApp.h"
#include "GDBController.h"
#include "GDBWindow.h"
#include "GIFView.h"
#ifdef BUILD_SDL
#include "input/SDLInputDriver.h"
#endif
#include "IOViewer.h"
#include "LoadSaveState.h"
#include "LogView.h"
#include "MapView.h"
#include "MemoryAccessLogView.h"
#include "MemorySearch.h"
#include "MemoryView.h"
#include "MultiplayerController.h"
#include "OverrideView.h"
#include "ObjView.h"
#include "PaletteView.h"
#include "PlacementControl.h"
#include "PrinterView.h"
#include "ReportView.h"
#include "ROMInfo.h"
#include "SaveConverter.h"
#ifdef ENABLE_SCRIPTING
#include "scripting/ScriptingView.h"
#endif
#include "SensorView.h"
#include "ShaderSelector.h"
#include "ShortcutController.h"
#include "TileView.h"
#include "VideoProxy.h"
#include "VideoView.h"

#ifdef USE_DISCORD_RPC
#include "DiscordCoordinator.h"
#endif

#include <mgba/core/version.h>
#include <mgba/core/serialize.h>
#include <mgba/core/cheats.h>
#ifdef M_CORE_GB
#include <mgba/internal/gb/gb.h>
#include <mgba/internal/gb/video.h>
#endif
#ifdef M_CORE_GBA
#include <mgba/gba/interface.h>
#include <mgba/internal/gba/gba.h>
#endif
#include <mgba/feature/commandline.h>
#include <mgba/internal/gba/input.h>
#include <mgba-util/vfs.h>

#include <mgba-util/convolve.h>

#include "moc_Window.cpp"

using namespace QGBA;

namespace {

struct FlashGBXProcessResult {
	int exitCode = -1;
	QString output;
};

struct FlashGBXLoadResult {
	bool success = false;
	bool saveBackedUp = false;
	bool saveUploadArmed = false;
	qint64 savePayloadSize = 0;
	QString error;
	QString output;
	QString mode;
	QString sessionDir;
	QString romPath;
	QString savePath;
	QString initialSavePath;
	QString initialSaveHash;
	QString flashcartType;
	QString saveType;
	QString dmgMbc;
	QString preloadWarning;
	QStringList restoreCommand;
};

struct FlashGBXUploadResult {
	bool success = false;
	QString error;
	QString output;
	QString saveHash;
	QString verifyHash;
	QString verifyPath;
};

QStringList splitCommandLine(const QString& command) {
	QStringList parts;
	QString current;
	QChar quote;
	bool escaped = false;
	for (QChar ch : command) {
		if (escaped) {
			current.append(ch);
			escaped = false;
			continue;
		}
		if (ch == QLatin1Char('\\')) {
			escaped = true;
			continue;
		}
		if (!quote.isNull()) {
			if (ch == quote) {
				quote = QChar();
			} else {
				current.append(ch);
			}
			continue;
		}
		if (ch == QLatin1Char('"') || ch == QLatin1Char('\'')) {
			quote = ch;
			continue;
		}
		if (ch.isSpace()) {
			if (!current.isEmpty()) {
				parts.append(current);
				current.clear();
			}
			continue;
		}
		current.append(ch);
	}
	if (escaped) {
		current.append(QLatin1Char('\\'));
	}
	if (!current.isEmpty()) {
		parts.append(current);
	}
	return parts;
}

QString shellJoin(const QStringList& command) {
	QStringList escaped;
	for (QString arg : command) {
		if (arg.isEmpty()) {
			escaped.append(QStringLiteral("''"));
			continue;
		}
		bool needsQuotes = false;
		for (QChar ch : arg) {
			if (ch.isSpace() || QStringLiteral("'\"\\$&;()<>|*?[]{}").contains(ch)) {
				needsQuotes = true;
				break;
			}
		}
		if (needsQuotes) {
			arg.replace(QLatin1Char('\''), QStringLiteral("'\\''"));
			escaped.append(QLatin1Char('\'') + arg + QLatin1Char('\''));
		} else {
			escaped.append(arg);
		}
	}
	return escaped.join(QLatin1Char(' '));
}

QString fileSha256(const QString& path) {
	QFile file(path);
	if (!file.open(QIODevice::ReadOnly)) {
		return QString();
	}
	QCryptographicHash hash(QCryptographicHash::Sha256);
	while (!file.atEnd()) {
		hash.addData(file.read(1024 * 1024));
	}
	return QString::fromLatin1(hash.result().toHex());
}

QString fileSha256Prefix(const QString& path, qint64 bytes) {
	if (bytes <= 0) {
		return fileSha256(path);
	}
	QFile file(path);
	if (!file.open(QIODevice::ReadOnly)) {
		return QString();
	}
	QCryptographicHash hash(QCryptographicHash::Sha256);
	qint64 remaining = bytes;
	while (remaining > 0 && !file.atEnd()) {
		const QByteArray chunk = file.read(qMin<qint64>(remaining, 1024 * 1024));
		if (chunk.isEmpty()) {
			break;
		}
		hash.addData(chunk);
		remaining -= chunk.size();
	}
	if (remaining != 0) {
		return QString();
	}
	return QString::fromLatin1(hash.result().toHex());
}

bool copyFilePrefix(const QString& sourcePath, const QString& destPath, qint64 bytes) {
	if (bytes <= 0) {
		QFile::remove(destPath);
		return QFile::copy(sourcePath, destPath);
	}
	QFile source(sourcePath);
	if (!source.open(QIODevice::ReadOnly)) {
		return false;
	}
	QFile dest(destPath);
	if (!dest.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
		return false;
	}
	qint64 remaining = bytes;
	while (remaining > 0) {
		const QByteArray chunk = source.read(qMin<qint64>(remaining, 1024 * 1024));
		if (chunk.isEmpty()) {
			return false;
		}
		if (dest.write(chunk) != chunk.size()) {
			return false;
		}
		remaining -= chunk.size();
	}
	return true;
}

QString savePathForRomPath(const QString& romPath) {
	const QFileInfo romInfo(romPath);
	return QDir(romInfo.dir()).filePath(romInfo.completeBaseName() + QStringLiteral(".sav"));
}

QByteArray readFilePrefix(const QString& path, qint64 maxSize) {
	QFile file(path);
	if (!file.open(QIODevice::ReadOnly)) {
		return QByteArray();
	}
	return file.read(maxSize);
}

bool bytesMatch(const QByteArray& data, int offset, const unsigned char* expected, int length) {
	if (data.size() < offset + length) {
		return false;
	}
	for (int i = 0; i < length; ++i) {
		if (static_cast<unsigned char>(data.at(offset + i)) != expected[i]) {
			return false;
		}
	}
	return true;
}

bool isUniformData(const QByteArray& data) {
	if (data.isEmpty()) {
		return true;
	}
	const char first = data.at(0);
	for (char byte : data) {
		if (byte != first) {
			return false;
		}
	}
	return true;
}

bool gbHeaderChecksumValid(const QByteArray& data) {
	if (data.size() < 0x14E) {
		return false;
	}
	int checksum = 0;
	for (int i = 0x134; i <= 0x14C; ++i) {
		checksum = checksum - static_cast<unsigned char>(data.at(i)) - 1;
	}
	return static_cast<unsigned char>(checksum) == static_cast<unsigned char>(data.at(0x14D));
}

bool gbaHeaderChecksumValid(const QByteArray& data) {
	if (data.size() < 0xBE) {
		return false;
	}
	int checksum = 0;
	for (int i = 0xA0; i <= 0xBC; ++i) {
		checksum += static_cast<unsigned char>(data.at(i));
	}
	return static_cast<unsigned char>(-checksum - 0x19) == static_cast<unsigned char>(data.at(0xBD));
}

bool gbRomHeaderHasRtc(const QByteArray& header) {
	if (header.size() <= 0x147) {
		return false;
	}
	switch (static_cast<unsigned char>(header.at(0x147))) {
	case 0x0F:
	case 0x10:
	case 0xFC:
	case 0xFD:
	case 0xFE:
		return true;
	default:
		return false;
	}
}

QString cartridgeRtcWarningKeyForRom(const QString& path, const QString& mode) {
	if (mode != QLatin1String("dmg")) {
		return QString();
	}
	const QByteArray header = readFilePrefix(path, 0x150);
	if (!gbHeaderChecksumValid(header) || !gbRomHeaderHasRtc(header)) {
		return QString();
	}

	QByteArray title = header.mid(0x134, 16);
	const int zero = title.indexOf('\0');
	if (zero >= 0) {
		title.truncate(zero);
	}
	const QString titleText = QString::fromLatin1(title.toHex());
	const QString mapper = QString::number(static_cast<unsigned char>(header.at(0x147)), 16);
	const QString checksum = QString::number(static_cast<unsigned char>(header.at(0x14E)), 16) +
	                         QString::number(static_cast<unsigned char>(header.at(0x14F)), 16);
	return QStringLiteral("dmg:%1:%2:%3").arg(titleText, mapper, checksum);
}

bool cartridgeRomLooksValid(const QString& path, const QString& mode, QString* reason = nullptr) {
	static const unsigned char gbLogo[] = {
		0xCE, 0xED, 0x66, 0x66, 0xCC, 0x0D, 0x00, 0x0B,
		0x03, 0x73, 0x00, 0x83, 0x00, 0x0C, 0x00, 0x0D,
		0x00, 0x08, 0x11, 0x1F, 0x88, 0x89, 0x00, 0x0E,
		0xDC, 0xCC, 0x6E, 0xE6, 0xDD, 0xDD, 0xD9, 0x99,
		0xBB, 0xBB, 0x67, 0x63, 0x6E, 0x0E, 0xEC, 0xCC,
		0xDD, 0xDC, 0x99, 0x9F, 0xBB, 0xB9, 0x33, 0x3E
	};
	static const unsigned char gbaLogo[] = {
		0x24, 0xFF, 0xAE, 0x51, 0x69, 0x9A, 0xA2, 0x21,
		0x3D, 0x84, 0x82, 0x0A, 0x84, 0xE4, 0x09, 0xAD,
		0x11, 0x24, 0x8B, 0x98, 0xC0, 0x81, 0x7F, 0x21,
		0xA3, 0x52, 0xBE, 0x19, 0x93, 0x09, 0xCE, 0x20,
		0x10, 0x46, 0x4A, 0x4A, 0xF8, 0x27, 0x31, 0xEC,
		0x58, 0xC7, 0xE8, 0x33, 0x82, 0xE3, 0xCE, 0xBF,
		0x85, 0xF4, 0xDF, 0x94, 0xCE, 0x4B, 0x09, 0xC1,
		0x94, 0x56, 0x8A, 0xC0, 0x13, 0x72, 0xA7, 0xFC,
		0x9F, 0x84, 0x4D, 0x73, 0xA3, 0xCA, 0x9A, 0x61,
		0x58, 0x97, 0xA3, 0x27, 0xFC, 0x03, 0x98, 0x76,
		0x23, 0x1D, 0xC7, 0x61, 0x03, 0x04, 0xAE, 0x56,
		0xBF, 0x38, 0x84, 0x00, 0x40, 0xA7, 0x0E, 0xFD,
		0xFF, 0x52, 0xFE, 0x03, 0x6F, 0x95, 0x30, 0xF1,
		0x97, 0xFB, 0xC0, 0x85, 0x60, 0xD6, 0x80, 0x25,
		0xA9, 0x63, 0xBE, 0x03, 0x01, 0x4E, 0x38, 0xE2,
		0xF9, 0xA2, 0x34, 0xFF, 0xBB, 0x3E, 0x03, 0x44,
		0x78, 0x00, 0x90, 0xCB, 0x88, 0x11, 0x3A, 0x94,
		0x65, 0xC0, 0x7C, 0x63, 0x87, 0xF0, 0x3C, 0xAF,
		0xD6, 0x25, 0xE4, 0x8B, 0x38, 0x0A, 0xAC, 0x72,
		0x21, 0xD4, 0xF8, 0x07
	};

	const QByteArray header = readFilePrefix(path, 0x200);
	if (isUniformData(header)) {
		if (reason) {
			*reason = QObject::tr("The cartridge reader returned blank data.");
		}
		return false;
	}

	const bool isGB = mode == QLatin1String("dmg");
	const int logoOffset = isGB ? 0x104 : 0x04;
	const unsigned char* logo = isGB ? gbLogo : gbaLogo;
	const int logoLength = isGB ? int(sizeof(gbLogo)) : int(sizeof(gbaLogo));
	if (!bytesMatch(header, logoOffset, logo, logoLength)) {
		if (reason) {
			*reason = QObject::tr("The cartridge ROM header was not recognized.");
		}
		return false;
	}

	const bool checksumValid = isGB ? gbHeaderChecksumValid(header) : gbaHeaderChecksumValid(header);
	if (!checksumValid) {
		if (reason) {
			*reason = QObject::tr("The cartridge ROM header checksum was invalid.");
		}
		return false;
	}
	return true;
}

QString appResourcesPath() {
	QStringList candidates;
	QDir dir(QCoreApplication::applicationDirPath());
	if (dir.dirName() == QLatin1String("MacOS") && dir.cdUp() && dir.dirName() == QLatin1String("Contents") && dir.cd(QStringLiteral("Resources"))) {
		candidates.append(dir.absolutePath());
	}
	candidates.append(GBAApp::dataDir());
	candidates.append(QCoreApplication::applicationDirPath());

	QStringList seen;
	for (const QString& candidate : candidates) {
		if (candidate.isEmpty()) {
			continue;
		}
		const QString cleanPath = QDir::cleanPath(candidate);
		if (seen.contains(cleanPath)) {
			continue;
		}
		seen.append(cleanPath);
		if (QFileInfo::exists(QDir(cleanPath).filePath(QStringLiteral("FlashGBX")))) {
			return cleanPath;
		}
	}
	return seen.isEmpty() ? QCoreApplication::applicationDirPath() : seen.first();
}

QStringList executableSearchPaths() {
	QStringList paths = QProcessEnvironment::systemEnvironment().value(QStringLiteral("PATH")).split(QDir::listSeparator(), Qt::SkipEmptyParts);
#ifndef Q_OS_WIN
	for (const QString& path : {QStringLiteral("/opt/homebrew/bin"), QStringLiteral("/usr/local/bin"), QStringLiteral("/usr/bin"), QStringLiteral("/bin")}) {
		if (!paths.contains(path)) {
			paths.prepend(path);
		}
	}
#endif
	return paths;
}

QString resolveExecutable(const QString& program) {
	if (program.contains(QLatin1Char('/'))) {
		return QFileInfo::exists(program) ? program : QString();
	}
	const QString resolved = QStandardPaths::findExecutable(program, executableSearchPaths());
	return resolved.isEmpty() ? QString() : resolved;
}

QString embeddedFlashGBXCommand() {
	const QDir resources(appResourcesPath());
	const QStringList standaloneCandidates{
		resources.filePath(QStringLiteral("FlashGBX/flashgbx-cli/flashgbx-cli")),
		resources.filePath(QStringLiteral("FlashGBX/flashgbx-cli/flashgbx-cli.exe")),
	};
	for (const QString& standalone : standaloneCandidates) {
		if (QFileInfo::exists(standalone)) {
			return shellJoin(QStringList{standalone, QStringLiteral("--cfgdir"), QStringLiteral("subdir")});
		}
	}
	const QString runner = resources.filePath(QStringLiteral("FlashGBX/flashgbx-bundled-runner.py"));
	if (QFileInfo::exists(runner)) {
#ifdef Q_OS_WIN
		const QStringList pythonCandidates{
			resolveExecutable(QStringLiteral("python3.exe")),
			resolveExecutable(QStringLiteral("python.exe")),
			resolveExecutable(QStringLiteral("py.exe")),
		};
#else
		const QStringList pythonCandidates{
			QStringLiteral("/opt/homebrew/bin/python3"),
			QStringLiteral("/usr/local/bin/python3"),
			QStringLiteral("/usr/bin/python3"),
		};
#endif
		for (const QString& python : pythonCandidates) {
			if (!python.isEmpty() && QFileInfo::exists(python)) {
				return shellJoin(QStringList{python, runner, QStringLiteral("--cfgdir"), QStringLiteral("subdir")});
			}
		}
	}
	return QString();
}

QString defaultFlashGBXCommand() {
	return embeddedFlashGBXCommand();
}

QStringList flashGBXDevicePorts() {
	QStringList ports;
#ifdef Q_OS_WIN
	for (int i = 1; i <= 256; ++i) {
		const QString port = QStringLiteral("COM%1").arg(i);
		wchar_t target[512];
		if (QueryDosDeviceW(reinterpret_cast<LPCWSTR>(port.utf16()), target, sizeof(target) / sizeof(target[0]))) {
			ports.append(port);
		}
	}
#else
	auto appendDeviceEntries = [&ports](const QString& path, const QStringList& patterns) {
		QDir devDir(path);
		for (const QString& entry : devDir.entryList(patterns, QDir::System | QDir::Files | QDir::NoDotAndDotDot, QDir::Name)) {
			ports.append(devDir.absoluteFilePath(entry));
		}
	};
	appendDeviceEntries(QStringLiteral("/dev"), QStringList{
		QStringLiteral("cu.*"),
		QStringLiteral("tty.*"),
		QStringLiteral("ttyACM*"),
		QStringLiteral("ttyUSB*"),
		QStringLiteral("ttyS*"),
	});
	appendDeviceEntries(QStringLiteral("/dev/serial/by-id"), QStringList{QStringLiteral("*")});
	appendDeviceEntries(QStringLiteral("/dev/serial/by-path"), QStringList{QStringLiteral("*")});
#endif
	ports.removeDuplicates();
	ports.sort();
	return ports;
}

QStringList buildFlashGBXCommand(const QStringList& base, const QString& mode, const QString& action,
                                 const QString& path, const QStringList& extra = {}) {
	QStringList command = base;
	command << QStringLiteral("--cli")
	        << QStringLiteral("--mode") << mode
	        << QStringLiteral("--action") << action
	        << QStringLiteral("--overwrite");
	command << extra;
	command << path;
	return command;
}

FlashGBXProcessResult runProcess(const QStringList& command) {
	FlashGBXProcessResult result;
	if (command.isEmpty()) {
		result.output = QStringLiteral("Empty command");
		return result;
	}

	QProcess process;
	const QString program = resolveExecutable(command.first());
	if (program.isEmpty()) {
		result.output = QStringLiteral("Could not find executable: %1\nSearched PATH: %2")
		                    .arg(command.first(), executableSearchPaths().join(QDir::listSeparator()));
		return result;
	}

	QProcessEnvironment environment = QProcessEnvironment::systemEnvironment();
	environment.insert(QStringLiteral("PATH"), executableSearchPaths().join(QDir::listSeparator()));
	process.setProcessEnvironment(environment);
	process.setProgram(program);
	process.setArguments(command.mid(1));
	process.setProcessChannelMode(QProcess::MergedChannels);
	process.start();
	if (!process.waitForStarted()) {
		result.output = process.errorString();
		return result;
	}
	process.waitForFinished(-1);
	result.exitCode = process.exitStatus() == QProcess::NormalExit ? process.exitCode() : -1;
	result.output = QString::fromLocal8Bit(process.readAllStandardOutput());
	if (result.output.isEmpty() && process.exitStatus() != QProcess::NormalExit) {
		result.output = process.errorString();
	}
	return result;
}

bool flashGBXActionSucceeded(const QString& action, const FlashGBXProcessResult& result, const QString& expectedPath = QString(),
                             const QString& mode = QString(), QString* reason = nullptr) {
	if (result.exitCode != 0) {
		return false;
	}
	if (action == QLatin1String("backup-rom")) {
		return cartridgeRomLooksValid(expectedPath, mode, reason);
	}
	if (action == QLatin1String("backup-save")) {
		return QFileInfo::exists(expectedPath) || result.output.contains(QStringLiteral("The save data backup is complete"));
	}
	if (action == QLatin1String("restore-save")) {
		return result.output.contains(QStringLiteral("The save data was restored"));
	}
	return false;
}

QString displayProcessOutput(const FlashGBXProcessResult& result) {
	QStringList lines = result.output.split(QLatin1Char('\n'));
	while (!lines.isEmpty() && (lines.first().startsWith(QStringLiteral("FlashGBX v")) ||
	                           lines.first().startsWith(QStringLiteral("https://github.com/Lesserkuma/FlashGBX")) ||
	                           lines.first().trimmed().isEmpty())) {
		lines.removeFirst();
	}
	QString output = lines.join(QLatin1Char('\n'));
	output.replace(QStringLiteral("FlashGBX"), QStringLiteral("cartridge reader"));
	return output.trimmed();
}

void appendProcessOutput(QString* output, const QString& action, const FlashGBXProcessResult& result) {
	output->append(QStringLiteral("Action: %1\n").arg(action));
	if (!result.output.isEmpty()) {
		const QString displayOutput = displayProcessOutput(result);
		if (!displayOutput.isEmpty()) {
			output->append(displayOutput);
			output->append(QLatin1Char('\n'));
		}
	}
}

bool flashGBXOutputHasRtcBatteryWarning(const QString& output, const QString& mode, const QString& dmgMbc, QString* rtcLineOut = nullptr) {
	QString rtcLine;
	QString mapperLine;
	for (const QString& rawLine : output.split(QLatin1Char('\n'))) {
		const QString line = rawLine.trimmed();
		if (line.startsWith(QStringLiteral("Real Time Clock:"), Qt::CaseInsensitive)) {
			rtcLine = line;
		} else if (line.startsWith(QStringLiteral("Mapper Type:"), Qt::CaseInsensitive)) {
			mapperLine = line;
		}
	}

	if (rtcLineOut) {
		*rtcLineOut = rtcLine;
	}

	const QString lowerMbc = dmgMbc.toLower();
	const bool selectedRtcMapper = mode == QLatin1String("dmg") &&
	                               (lowerMbc == QLatin1String("0x10") ||
	                                lowerMbc == QLatin1String("0x110") ||
	                                lowerMbc == QLatin1String("0xfe") ||
	                                lowerMbc == QLatin1String("0xfd"));
	const bool detectedRtcMapper = mapperLine.contains(QStringLiteral("RTC"), Qt::CaseInsensitive);
	const bool rtcExpected = selectedRtcMapper || detectedRtcMapper;
	const bool badRtc = rtcLine.contains(QStringLiteral("Not available"), Qt::CaseInsensitive) ||
	                    rtcLine.contains(QStringLiteral("Invalid"), Qt::CaseInsensitive) ||
	                    rtcLine.contains(QStringLiteral("Battery dry"), Qt::CaseInsensitive);
	bool resetLookingRtc = false;
	const QRegularExpression rtcTimePattern(QStringLiteral(R"(Real Time Clock:\s+(\d+)\s+days?,\s+(\d+):(\d+):(\d+))"),
	                                        QRegularExpression::CaseInsensitiveOption);
	const QRegularExpressionMatch rtcTimeMatch = rtcTimePattern.match(rtcLine);
	if (rtcTimeMatch.hasMatch()) {
		const int days = rtcTimeMatch.captured(1).toInt();
		resetLookingRtc = days == 0;
	}
	const bool unstableSave = output.contains(QStringLiteral("not battery-backed"), Qt::CaseInsensitive) ||
	                          output.contains(QStringLiteral("cartridge connection is unstable"), Qt::CaseInsensitive);
	return (rtcExpected && (badRtc || resetLookingRtc)) || unstableSave;
}

QString flashGBXRtcBatteryWarning(const QString& output, const QString& mode, const QString& dmgMbc) {
	QString rtcLine;
	if (!flashGBXOutputHasRtcBatteryWarning(output, mode, dmgMbc, &rtcLine)) {
		return QString();
	}

	QString detail;
	if (!rtcLine.isEmpty()) {
		detail = QObject::tr("\n\nReader status: %1").arg(rtcLine);
	}
	return QObject::tr("RTC or save battery issue suspected.\n\nThis cartridge appears to use a battery-backed RTC/save circuit, but the cartridge reader could not read stable RTC data or reported reset-looking RTC data. The game may report that no save exists, or new saves may not persist.%1\n\nDo you want to load the ROM anyway?")
	    .arg(detail);
}

QString rememberedFlashGBXRtcBatteryWarning() {
	return QObject::tr("RTC or save battery issue suspected.\n\nThis cartridge previously reported unstable RTC/save battery data. The game may report that no save exists, or new saves may not persist.\n\nDo you want to load the ROM anyway?");
}

}

Window::Window(CoreManager* manager, ConfigController* config, int playerId, QWidget* parent)
	: QMainWindow(parent)
	, m_manager(manager)
	, m_logView(new LogView(&m_log, this))
	, m_screenWidget(new WindowBackground())
	, m_config(config)
	, m_inputController(this)
	, m_shortcutController(new ShortcutController(this))
	, m_playerId(playerId)
{
	setFocusPolicy(Qt::StrongFocus);
	setAcceptDrops(true);
	setAttribute(Qt::WA_DeleteOnClose);
	updateTitle();

	m_logo.setDevicePixelRatio(m_screenWidget->devicePixelRatio());
	m_logo = m_logo; // Free memory left over in old pixmap

#if defined(M_CORE_GBA)
	float i = 2;
#elif defined(M_CORE_GB)
	float i = 3;
#endif
	QVariant multiplier = m_config->getOption("scaleMultiplier");
	if (!multiplier.isNull()) {
		m_savedScale = multiplier.toInt();
		i = m_savedScale;
	}
#ifdef USE_SQLITE3
	m_libraryView = new LibraryController(nullptr, ConfigController::configDir() + "/library.sqlite3", m_config);
	ConfigOption* showLibrary = m_config->addOption("showLibrary");
	showLibrary->connect([this](const QVariant& value) {
		if (!m_controller) {
			if (value.toBool()) {
				attachWidget(m_libraryView);
			} else {
				attachWidget(m_screenWidget);
			}
		}
	}, this);
	m_config->updateOption("showLibrary");

	ConfigOption* showFilenameInLibrary = m_config->addOption("showFilenameInLibrary");
	showFilenameInLibrary->connect([this](const QVariant& value) {
			m_libraryView->setShowFilename(value.toBool());
	}, this);
	m_config->updateOption("showFilenameInLibrary");
	ConfigOption* libraryStyle = m_config->addOption("libraryStyle");
	libraryStyle->connect([this](const QVariant& value) {
		m_libraryView->setViewStyle(static_cast<LibraryStyle>(value.toInt()));
	}, this);
	m_config->updateOption("libraryStyle");

	connect(m_libraryView, &LibraryController::startGame, [this]() {
		VFile* output = m_libraryView->selectedVFile();
		if (output) {
			QPair<QString, QString> path = m_libraryView->selectedPath();
			setController(m_manager->loadGame(output, path.second, path.first));
		}
	});
#endif
#if defined(M_CORE_GBA)
	QSize minimumSize = QSize(GBA_VIDEO_HORIZONTAL_PIXELS, GBA_VIDEO_VERTICAL_PIXELS);
#elif defined(M_CORE_GB)
	QSize minimumSize = QSize(GB_VIDEO_HORIZONTAL_PIXELS, GB_VIDEO_VERTICAL_PIXELS);
#endif
	setMinimumSize(minimumSize);
	if (i > 0) {
		m_initialSize = minimumSize * i;
	} else {
		m_initialSize = minimumSize * 2;
	}
	setLogo();

	connect(this, &Window::shutdown, m_logView, &QWidget::hide);
	connect(&m_fpsTimer, &QTimer::timeout, this, &Window::showFPS);
	connect(&m_focusCheck, &QTimer::timeout, this, &Window::focusCheck);
	connect(&m_inputController, &InputController::profileLoaded, m_shortcutController, &ShortcutController::loadProfile);

	m_log.setLevels(mLOG_WARN | mLOG_ERROR | mLOG_FATAL);
	m_log.load(m_config);
	m_fpsTimer.setInterval(FPS_TIMER_INTERVAL);
	m_focusCheck.setInterval(200);
	m_mustRestart.setInterval(MUST_RESTART_TIMEOUT);
	m_mustRestart.setSingleShot(true);
	m_mustReset.setInterval(MUST_RESTART_TIMEOUT);
	m_mustReset.setSingleShot(true);
	m_flashgbxSaveUploadTimer.setInterval(1500);
	m_flashgbxSaveUploadTimer.setSingleShot(true);
	connect(&m_flashgbxSaveUploadTimer, &QTimer::timeout, this, [this]() {
		uploadFlashGBXSave(false);
	});
	connect(&m_flashgbxSaveWatcher, &QFileSystemWatcher::fileChanged, this, [this](const QString&) {
		configureFlashGBXSaveWatcher();
		scheduleFlashGBXSaveUpload();
	});

#ifdef BUILD_SDL
	m_inputController.addInputDriver(std::make_shared<SDLInputDriver>(&m_inputController));
#if SDL_VERSION_ATLEAST(2, 0, 0)
	m_inputController.setGamepadDriver(SDL_BINDING_CONTROLLER);
	m_inputController.setSensorDriver(SDL_BINDING_CONTROLLER);
#else
	m_inputController.setGamepadDriver(SDL_BINDING_BUTTON);
	m_inputController.setSensorDriver(SDL_BINDING_BUTTON);
#endif
#endif

	m_shortcutController->setConfigController(m_config);
	m_shortcutController->setActionMapper(&m_actions);
	setupMenu(menuBar());
	setupOptions();
	setContextMenuPolicy(Qt::CustomContextMenu);
	connect(this, &QWidget::customContextMenuRequested, [this](const QPoint& pos) {
		m_actions.exec(mapToGlobal(pos));
	});
}

Window::~Window() {
	delete m_logView;

#ifdef USE_SQLITE3
	delete m_libraryView;
#endif
}

void Window::argumentsPassed() {
	const mArguments* args = m_config->args();

	if (args->patch) {
		m_pendingPatch = args->patch;
	}

	if (args->savestate) {
		m_pendingState = args->savestate;
	}

#ifdef ENABLE_GDB_STUB
	if (args->debugGdb) {
		if (!m_gdbController) {
			m_gdbController = new GDBController(this);
		}
		if (m_controller) {
			m_gdbController->setController(m_controller);
		}
		m_gdbController->attach();
		m_gdbController->listen();
	}
#endif

#ifdef ENABLE_DEBUGGERS
	if (args->debugCli) {
		consoleOpen();
	}
#endif

	if (m_config->graphicsOpts()->multiplier > 0) {
		m_savedScale = m_config->graphicsOpts()->multiplier;

#if defined(M_CORE_GBA)
		QSize size(GBA_VIDEO_HORIZONTAL_PIXELS, GBA_VIDEO_VERTICAL_PIXELS);
#elif defined(M_CORE_GB)
		QSize size(GB_VIDEO_HORIZONTAL_PIXELS, GB_VIDEO_VERTICAL_PIXELS);
#endif
		m_initialSize = size * m_savedScale;
	}

	if (args->fname) {
		setController(m_manager->loadGame(args->fname));
	}

	if (m_config->graphicsOpts()->fullscreen) {
		enterFullScreen();
	}
}

void Window::resizeFrame(const QSize& size) {
	QSize newSize(size);
	if (!m_config->getOption("lockFrameSize").toInt()) {
		m_savedSize = size;
	}
	if (windowHandle()) {
		QRect geom = windowHandle()->screen()->availableGeometry();
		if (newSize.width() > geom.width()) {
			newSize.setWidth(geom.width());
		}
		if (newSize.height() > geom.height()) {
			newSize.setHeight(geom.height());
		}
	}
	newSize += this->size();
	newSize -= centralWidget()->size();
	if (!isFullScreen()) {
		resize(newSize);
	}
}

void Window::updateMultiplayerStatus(bool canOpenAnother) {
	m_multiWindow->setEnabled(canOpenAnother);
	multiplayerChanged();
}

void Window::updateMultiplayerActive(bool active) {
	m_multiActive = active;
	updateMute();
}

void Window::setConfig(ConfigController* config) {
	m_config = config;
}

void Window::loadConfig() {
	const mCoreOptions* opts = m_config->options();
	reloadConfig();

	if (opts->width && opts->height) {
		m_initialSize = QSize(opts->width, opts->height);
	}

	if (opts->fullscreen) {
		enterFullScreen();
	}

	m_mruFiles = m_config->getMRU();
	updateMRU();

	m_inputController.setConfiguration(m_config);

	if (!m_config->getList("autorunSettings").isEmpty()) {
		ensureScripting();
	}
}

void Window::reloadConfig() {
	const mCoreOptions* opts = m_config->options();

	m_log.setLevels(opts->logLevel);

	if (m_controller) {
		m_controller->loadConfig(m_config);
		if (m_audioProcessor) {
			m_audioProcessor->configure(m_config);
		}
		updateMute();
		m_display->resizeContext();
	}

	GBAApp::app()->setScreensaverSuspendable(opts->suspendScreensaver);
}

void Window::saveConfig() {
	m_inputController.saveConfiguration();
	m_config->write();
}

QString Window::getFiltersArchive() const {
	QStringList filters;

	QStringList formats{
#if defined(USE_LIBZIP) || defined(USE_MINIZIP)
		"*.zip",
#endif
#ifdef USE_LZMA
		"*.7z",
#endif
	};
	filters.append(tr("Archives (%1)").arg(formats.join(QChar(' '))));
	return filters.join(";;");
}

void Window::selectROM() {
	if (blockFlashGBXSaveUploadInProgress()) {
		return;
	}
	QString filename = GBAApp::app()->getOpenFileName(this, tr("Select ROM"), romFilters(true));
	if (!filename.isEmpty()) {
		setController(m_manager->loadGame(filename));
	}
}

void Window::selectFlashGBXCartridge() {
	if (blockFlashGBXSaveUploadInProgress()) {
		return;
	}
	if (m_flashgbxBusy) {
		showFlashGBXOverlayWarning(tr("A cartridge operation is already running."));
		return;
	}

	QDialog dialog(this, Qt::Sheet);
	dialog.setWindowTitle(tr("Load Cartridge"));
	QFormLayout form(&dialog);

	QWidget modeWidget(&dialog);
	QHBoxLayout* modeLayout = new QHBoxLayout(&modeWidget);
	modeLayout->setContentsMargins(0, 0, 0, 0);
	QRadioButton modeDMG(tr("GB/GBC"), &modeWidget);
	QRadioButton modeAGB(tr("GBA"), &modeWidget);
	modeLayout->addWidget(&modeDMG);
	modeLayout->addWidget(&modeAGB);
	modeLayout->addStretch(1);
	QString savedMode = m_config->getQtOption(QStringLiteral("mode"), QStringLiteral("flashgbx")).toString();
	if (savedMode == QLatin1String("dmg")) {
		modeDMG.setChecked(true);
	} else {
		modeAGB.setChecked(true);
	}

	QComboBox devicePort(&dialog);
	devicePort.setEditable(true);
	auto populateDevicePorts = [&devicePort](const QString& selectedPort) {
		devicePort.clear();
		devicePort.addItem(Window::tr("Auto-detect"), QString());
		for (const QString& port : flashGBXDevicePorts()) {
			devicePort.addItem(port, port);
		}
		if (!selectedPort.isEmpty()) {
			int portIndex = devicePort.findText(selectedPort);
			if (portIndex < 0) {
				devicePort.addItem(selectedPort, selectedPort);
				portIndex = devicePort.findText(selectedPort);
			}
			devicePort.setCurrentIndex(portIndex);
		}
	};
	populateDevicePorts(m_config->getQtOption(QStringLiteral("devicePort"), QStringLiteral("flashgbx")).toString());

	QPushButton detectPorts(tr("Detect"), &dialog);
	QHBoxLayout* portLayout = new QHBoxLayout;
	portLayout->addWidget(&devicePort, 1);
	portLayout->addWidget(&detectPorts);
	connect(&detectPorts, &QPushButton::clicked, &dialog, [&devicePort, populateDevicePorts]() {
		const QString selectedPort = devicePort.currentText().trimmed();
		populateDevicePorts(selectedPort == devicePort.itemText(0) ? QString() : selectedPort);
	});

	QComboBox flashcartType(&dialog);
	flashcartType.setEditable(true);
	flashcartType.addItem(tr("Auto-detect"), QStringLiteral("autodetect"));
	QString savedFlashcartType = m_config->getQtOption(QStringLiteral("flashcartType"), QStringLiteral("flashgbx")).toString();
	if (savedFlashcartType.isEmpty()) {
		savedFlashcartType = QStringLiteral("autodetect");
	}
	if (savedFlashcartType == QLatin1String("autodetect")) {
		flashcartType.setCurrentIndex(0);
	} else {
		int portIndex = flashcartType.findText(savedFlashcartType);
		if (portIndex < 0) {
			flashcartType.addItem(savedFlashcartType, savedFlashcartType);
			portIndex = flashcartType.findText(savedFlashcartType);
		}
		flashcartType.setCurrentIndex(portIndex);
	}

	auto setComboByValue = [](QComboBox& combo, const QString& selectedValue) {
		QString value = selectedValue.trimmed();
		if (value.isEmpty()) {
			value = QStringLiteral("auto");
		}
		int index = combo.findData(value);
		if (index < 0) {
			index = combo.findText(value);
		}
		if (index < 0) {
			combo.addItem(value, value);
			index = combo.count() - 1;
		}
		combo.setCurrentIndex(index);
	};
	auto comboValue = [](const QComboBox& combo, const QString& autoDetectLabel, const QString& defaultValue) {
		const QString text = combo.currentText().trimmed();
		const int index = combo.currentIndex();
		if (index >= 0 && text == combo.itemText(index).trimmed()) {
			const QString value = combo.itemData(index).toString().trimmed();
			if (!value.isEmpty()) {
				return value;
			}
		}
		if (text.isEmpty() || text == autoDetectLabel) {
			return defaultValue;
		}
		return text;
	};

	QComboBox saveType(&dialog);
	saveType.setEditable(true);
	auto populateSaveTypes = [&saveType, &setComboByValue, this](bool dmgMode) {
		saveType.clear();
		saveType.addItem(tr("Auto-detect"), QStringLiteral("auto"));
		if (dmgMode) {
			saveType.addItem(tr("4K SRAM (512 Bytes)"), QStringLiteral("4k"));
			saveType.addItem(tr("16K SRAM (2 KiB)"), QStringLiteral("16k"));
			saveType.addItem(tr("64K SRAM (8 KiB)"), QStringLiteral("64k"));
			saveType.addItem(tr("256K SRAM (32 KiB)"), QStringLiteral("256k"));
			saveType.addItem(tr("512K SRAM (64 KiB)"), QStringLiteral("512k"));
			saveType.addItem(tr("1M SRAM (128 KiB)"), QStringLiteral("1m"));
			saveType.addItem(tr("MBC6 SRAM+FLASH (1.03 MiB)"), QStringLiteral("mbc6"));
			saveType.addItem(tr("MBC7 2K EEPROM (256 Bytes)"), QStringLiteral("mbc7_2k"));
			saveType.addItem(tr("MBC7 4K EEPROM (512 Bytes)"), QStringLiteral("mbc7_4k"));
			saveType.addItem(tr("TAMA5 EEPROM (32 Bytes)"), QStringLiteral("tama5"));
			saveType.addItem(tr("Unlicensed 4M SRAM (512 KiB)"), QStringLiteral("sram4m"));
			saveType.addItem(tr("Unlicensed 1M EEPROM (128 KiB)"), QStringLiteral("eeprom1m"));
			saveType.addItem(tr("Unlicensed Photo! Directory (1 MiB)"), QStringLiteral("photo"));
			setComboByValue(saveType, m_config->getQtOption(QStringLiteral("dmgSaveType"), QStringLiteral("flashgbx")).toString());
		} else {
			saveType.addItem(tr("4K EEPROM (512 Bytes)"), QStringLiteral("eeprom4k"));
			saveType.addItem(tr("64K EEPROM (8 KiB)"), QStringLiteral("eeprom64k"));
			saveType.addItem(tr("256K SRAM/FRAM (32 KiB)"), QStringLiteral("sram256k"));
			saveType.addItem(tr("512K FLASH (64 KiB)"), QStringLiteral("flash512k"));
			saveType.addItem(tr("1M FLASH (128 KiB)"), QStringLiteral("flash1m"));
			saveType.addItem(tr("8M DACS (1 MiB)"), QStringLiteral("dacs8m"));
			saveType.addItem(tr("Unlicensed 512K SRAM (64 KiB)"), QStringLiteral("sram512k"));
			saveType.addItem(tr("Unlicensed 1M SRAM (128 KiB)"), QStringLiteral("sram1m"));
			setComboByValue(saveType, m_config->getQtOption(QStringLiteral("agbSaveType"), QStringLiteral("flashgbx")).toString());
		}
	};
	populateSaveTypes(modeDMG.isChecked());

	QComboBox dmgMbc(&dialog);
	dmgMbc.setEditable(true);
	dmgMbc.addItem(tr("Auto-detect"), QStringLiteral("auto"));
	dmgMbc.addItem(tr("None"), QStringLiteral("0x00"));
	dmgMbc.addItem(tr("MBC1"), QStringLiteral("1"));
	dmgMbc.addItem(tr("MBC2"), QStringLiteral("2"));
	dmgMbc.addItem(tr("MBC3 + RTC + SRAM + Battery"), QStringLiteral("0x10"));
	dmgMbc.addItem(tr("MBC3 + SRAM + Battery"), QStringLiteral("0x13"));
	dmgMbc.addItem(tr("MBC5"), QStringLiteral("5"));
	dmgMbc.addItem(tr("MBC5 + SRAM + Battery"), QStringLiteral("0x1B"));
	dmgMbc.addItem(tr("MBC5 + Rumble + SRAM + Battery"), QStringLiteral("0x1E"));
	dmgMbc.addItem(tr("MBC6"), QStringLiteral("6"));
	dmgMbc.addItem(tr("MBC7"), QStringLiteral("7"));
	dmgMbc.addItem(tr("MBC30 + RTC + SRAM + Battery"), QStringLiteral("0x110"));
	dmgMbc.addItem(tr("MBC1M"), QStringLiteral("0x101"));
	dmgMbc.addItem(tr("HuC-1"), QStringLiteral("0xFF"));
	dmgMbc.addItem(tr("HuC-3"), QStringLiteral("0xFE"));
	dmgMbc.addItem(tr("TAMA5"), QStringLiteral("0xFD"));
	setComboByValue(dmgMbc, m_config->getQtOption(QStringLiteral("dmgMbc"), QStringLiteral("flashgbx")).toString());
	dmgMbc.setEnabled(modeDMG.isChecked());
	connect(&modeDMG, &QRadioButton::toggled, &dialog, [&populateSaveTypes, &dmgMbc](bool checked) {
		if (checked) {
			populateSaveTypes(true);
		}
		dmgMbc.setEnabled(checked);
	});
	connect(&modeAGB, &QRadioButton::toggled, &dialog, [&populateSaveTypes](bool checked) {
		if (checked) {
			populateSaveTypes(false);
		}
	});

	QCheckBox autoUpload(tr("Upload save changes to the cartridge automatically"), &dialog);
	QVariant savedAutoUpload = m_config->getQtOption(QStringLiteral("autoUpload"), QStringLiteral("flashgbx"));
	autoUpload.setChecked(savedAutoUpload.isNull() ? true : savedAutoUpload.toBool());

	form.addRow(tr("Cartridge type:"), &modeWidget);
	form.addRow(tr("Serial port:"), portLayout);
	form.addRow(tr("Cartridge profile:"), &flashcartType);
	form.addRow(tr("Save type:"), &saveType);
	form.addRow(tr("GB/GBC memory controller:"), &dmgMbc);
	form.addRow(QString(), &autoUpload);

	QLabel hint(tr("The cartridge reader is bundled with mGBA. Port auto-detect leaves the serial port unset; cartridge profile auto-detect uses the reader's autodetect mode. If save detection is unstable, choose the cartridge's exact save type."), &dialog);
	hint.setWordWrap(true);
	form.addRow(&hint);

	QDialogButtonBox buttons(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, &dialog);
	form.addRow(&buttons);
	connect(&buttons, &QDialogButtonBox::accepted, &dialog, &QDialog::accept);
	connect(&buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);

	if (dialog.exec() != QDialog::Accepted) {
		return;
	}

	const QString flashgbxCommand = defaultFlashGBXCommand();
	const QString cartridgeMode = modeDMG.isChecked() ? QStringLiteral("dmg") : QStringLiteral("agb");
	const QString autoDetectLabel = tr("Auto-detect");
	QString port = devicePort.currentText().trimmed();
	if (port == autoDetectLabel) {
		port.clear();
	}
	QString flashcartTypeName = flashcartType.currentText().trimmed();
	if (flashcartTypeName.isEmpty() || flashcartTypeName == autoDetectLabel) {
		flashcartTypeName = QStringLiteral("autodetect");
	}
	const QString saveTypeName = comboValue(saveType, autoDetectLabel, QStringLiteral("auto"));
	const QString dmgMbcName = comboValue(dmgMbc, autoDetectLabel, QStringLiteral("auto"));
	const bool autoUploadEnabled = autoUpload.isChecked();

	m_config->setQtOption(QStringLiteral("mode"), cartridgeMode, QStringLiteral("flashgbx"));
	m_config->setQtOption(QStringLiteral("devicePort"), port, QStringLiteral("flashgbx"));
	m_config->setQtOption(QStringLiteral("flashcartType"), flashcartTypeName, QStringLiteral("flashgbx"));
	m_config->setQtOption(cartridgeMode == QLatin1String("dmg") ? QStringLiteral("dmgSaveType") : QStringLiteral("agbSaveType"), saveTypeName, QStringLiteral("flashgbx"));
	m_config->setQtOption(QStringLiteral("dmgMbc"), dmgMbcName, QStringLiteral("flashgbx"));
	m_config->setQtOption(QStringLiteral("autoUpload"), autoUploadEnabled, QStringLiteral("flashgbx"));
	m_config->write();

	if (flashgbxCommand.isEmpty()) {
		showFlashGBXOverlayWarning(tr("The embedded cartridge reader could not be found."));
		return;
	}

	QProgressDialog* progress = new QProgressDialog(tr("Reading cartridge..."), QString(), 0, 0, this, Qt::Sheet);
	progress->setCancelButton(nullptr);
	progress->setWindowModality(Qt::WindowModal);
	progress->setAttribute(Qt::WA_DeleteOnClose);
	progress->show();

	m_flashgbxBusy = true;
	auto result = std::make_shared<FlashGBXLoadResult>();
	GBAApp::app()->submitWorkerJob([result, flashgbxCommand, cartridgeMode, port, flashcartTypeName, saveTypeName, dmgMbcName]() {
		QStringList baseCommand = splitCommandLine(flashgbxCommand);
		if (baseCommand.isEmpty()) {
			result->error = QObject::tr("The embedded cartridge reader could not be started.");
			return;
		}

		QStringList extraArgs;
		if (!port.isEmpty()) {
			extraArgs << QStringLiteral("--device-port") << port;
		}
		if (cartridgeMode == QLatin1String("dmg")) {
			extraArgs << QStringLiteral("--dmg-savetype") << (saveTypeName.isEmpty() ? QStringLiteral("auto") : saveTypeName);
			extraArgs << QStringLiteral("--dmg-mbc") << (dmgMbcName.isEmpty() ? QStringLiteral("auto") : dmgMbcName);
		} else {
			extraArgs << QStringLiteral("--agb-savetype") << (saveTypeName.isEmpty() ? QStringLiteral("auto") : saveTypeName);
		}
		extraArgs << QStringLiteral("--flashcart-type") << (flashcartTypeName.isEmpty() ? QStringLiteral("autodetect") : flashcartTypeName);

		const QString suffix = cartridgeMode == QLatin1String("dmg") ? QStringLiteral("gb") : QStringLiteral("gba");
		const QString sessionRoot = QDir(ConfigController::configDir()).filePath(QStringLiteral("CartridgeReader"));
		const QString sessionName = QDateTime::currentDateTime().toString(QStringLiteral("yyyyMMdd-hhmmss"));
		const QString sessionDir = QDir(sessionRoot).filePath(sessionName);
		QDir().mkpath(sessionDir);

		result->mode = cartridgeMode;
		result->sessionDir = sessionDir;
		result->romPath = QDir(sessionDir).filePath(QStringLiteral("cart.") + suffix);
		result->savePath = savePathForRomPath(result->romPath);
		result->initialSavePath = QDir(sessionDir).filePath(QStringLiteral("cart.initial.sav"));
		const QString verifySavePath = QDir(sessionDir).filePath(QStringLiteral("cart.verify.sav"));
		result->flashcartType = flashcartTypeName;
		result->saveType = saveTypeName;
		result->dmgMbc = cartridgeMode == QLatin1String("dmg") ? dmgMbcName : QString();
		result->restoreCommand = buildFlashGBXCommand(baseCommand, cartridgeMode, QStringLiteral("restore-save"), result->savePath, extraArgs);

		QString combinedOutput;
		QStringList infoCommand = buildFlashGBXCommand(baseCommand, cartridgeMode, QStringLiteral("info"), QStringLiteral("auto"), extraArgs);
		FlashGBXProcessResult infoResult = runProcess(infoCommand);
		appendProcessOutput(&combinedOutput, QStringLiteral("info"), infoResult);
		result->preloadWarning = flashGBXRtcBatteryWarning(infoResult.output, cartridgeMode, dmgMbcName);

		QFile::remove(result->savePath);
		QFile::remove(verifySavePath);
		QStringList backupSave = buildFlashGBXCommand(baseCommand, cartridgeMode, QStringLiteral("backup-save"), result->savePath, extraArgs);
		FlashGBXProcessResult backupSaveResult = runProcess(backupSave);
		appendProcessOutput(&combinedOutput, QStringLiteral("backup-save"), backupSaveResult);
		if (result->preloadWarning.isEmpty()) {
			result->preloadWarning = flashGBXRtcBatteryWarning(backupSaveResult.output, cartridgeMode, dmgMbcName);
		}
		if (!flashGBXActionSucceeded(QStringLiteral("backup-save"), backupSaveResult, result->savePath)) {
			result->error = QObject::tr("Could not back up the cartridge save data. The ROM was not loaded to avoid starting without the cartridge save.");
			result->output = combinedOutput;
			return;
		}
		const QFileInfo saveInfo(result->savePath);
		const QString saveHash = fileSha256(result->savePath);
		if (!saveInfo.exists() || saveInfo.size() <= 0 || saveHash.isEmpty()) {
			result->error = QObject::tr("Cartridge save data could not be read. The ROM was not loaded.");
			result->output = combinedOutput;
			return;
		}

		QStringList verifySave = buildFlashGBXCommand(baseCommand, cartridgeMode, QStringLiteral("backup-save"), verifySavePath, extraArgs);
		FlashGBXProcessResult verifySaveResult = runProcess(verifySave);
		appendProcessOutput(&combinedOutput, QStringLiteral("verify-save"), verifySaveResult);
		if (result->preloadWarning.isEmpty()) {
			result->preloadWarning = flashGBXRtcBatteryWarning(verifySaveResult.output, cartridgeMode, dmgMbcName);
		}
		const QString verifySaveHash = fileSha256(verifySavePath);
		if (!flashGBXActionSucceeded(QStringLiteral("backup-save"), verifySaveResult, verifySavePath) ||
		    verifySaveHash.isEmpty() || verifySaveHash != saveHash) {
			if (!combinedOutput.isEmpty()) {
				combinedOutput.append(QLatin1Char('\n'));
			}
			combinedOutput.append(QObject::tr("Initial save SHA-256: %1\nVerified save SHA-256: %2")
			                      .arg(saveHash, verifySaveHash.isEmpty() ? QObject::tr("(unreadable)") : verifySaveHash));
			result->error = QObject::tr("Cartridge save data could not be verified. The ROM was not loaded.");
			result->output = combinedOutput;
			return;
		}

		QFile::remove(result->initialSavePath);
		if (!QFile::copy(result->savePath, result->initialSavePath)) {
			result->error = QObject::tr("Could not preserve the verified cartridge save backup. The ROM was not loaded.");
			result->output = combinedOutput;
			return;
		}
		QFile::remove(verifySavePath);
		result->initialSaveHash = saveHash;
		result->savePayloadSize = saveInfo.size();
		result->saveUploadArmed = true;
		result->saveBackedUp = true;

		QStringList backupRom = buildFlashGBXCommand(baseCommand, cartridgeMode, QStringLiteral("backup-rom"), result->romPath, extraArgs);
		FlashGBXProcessResult backupRomResult = runProcess(backupRom);
		appendProcessOutput(&combinedOutput, QStringLiteral("backup-rom"), backupRomResult);
		if (result->preloadWarning.isEmpty()) {
			result->preloadWarning = flashGBXRtcBatteryWarning(backupRomResult.output, cartridgeMode, dmgMbcName);
		}
		QString romRejectReason;
		if (!flashGBXActionSucceeded(QStringLiteral("backup-rom"), backupRomResult, result->romPath, cartridgeMode, &romRejectReason)) {
			if (!romRejectReason.isEmpty()) {
				if (!combinedOutput.isEmpty()) {
					combinedOutput.append(QLatin1Char('\n'));
				}
				combinedOutput.append(romRejectReason);
				result->error = QObject::tr("No valid cartridge ROM was detected. Make sure the cartridge is inserted correctly.");
			} else {
				result->error = QObject::tr("Could not back up the cartridge ROM.");
			}
			result->output = combinedOutput;
			return;
		}

		result->output = combinedOutput;
		result->success = true;
	}, this, [this, progress, result, autoUploadEnabled]() {
		m_flashgbxBusy = false;
		progress->close();
		progress->deleteLater();

		if (!result->success) {
			showFlashGBXOverlayWarning(result->error.isEmpty() ? tr("Cartridge read failed.") : result->error);
			return;
		}

		const QString rtcWarningKey = cartridgeRtcWarningKeyForRom(result->romPath, result->mode);
		if (!rtcWarningKey.isEmpty()) {
			QStringList warnedCartridges = m_config->getQtOption(QStringLiteral("rtcWarningCartridges"), QStringLiteral("flashgbx")).toStringList();
			if (result->preloadWarning.isEmpty() && warnedCartridges.contains(rtcWarningKey)) {
				result->preloadWarning = rememberedFlashGBXRtcBatteryWarning();
			}
			if (!result->preloadWarning.isEmpty() && !warnedCartridges.contains(rtcWarningKey)) {
				warnedCartridges.append(rtcWarningKey);
				m_config->setQtOption(QStringLiteral("rtcWarningCartridges"), warnedCartridges, QStringLiteral("flashgbx"));
				m_config->write();
			}
		}

		if (!result->preloadWarning.isEmpty() && !confirmFlashGBXOverlayWarning(result->preloadWarning)) {
			QFile::remove(result->romPath);
			QFile::remove(result->savePath);
			QFile::remove(result->initialSavePath);
			QFile::remove(QDir(result->sessionDir).filePath(QStringLiteral("cart.verify.sav")));
			QFile::remove(QDir(result->sessionDir).filePath(QStringLiteral("cart.upload.sav")));
			QDir parentDir(QFileInfo(result->sessionDir).dir());
			parentDir.rmdir(QFileInfo(result->sessionDir).fileName());
			showFlashGBXOverlayWarning(tr("Cartridge load canceled."));
			return;
		}

		auto session = std::make_unique<FlashGBXSession>();
		session->mode = result->mode;
		session->sessionDir = result->sessionDir;
		session->romPath = result->romPath;
		session->savePath = result->savePath;
		session->initialSavePath = result->initialSavePath;
		session->initialSaveHash = result->initialSaveHash;
		session->savePayloadSize = result->savePayloadSize;
		session->flashcartType = result->flashcartType;
		session->saveType = result->saveType;
		session->dmgMbc = result->dmgMbc;
		session->preloadWarning = result->preloadWarning;
		session->restoreCommand = result->restoreCommand;
		session->saveUploadArmed = result->saveUploadArmed;
		m_flashgbxSession = std::move(session);
		m_flashgbxQueuedSaveHash.clear();
		m_flashgbxUploadingSaveHash.clear();
		writeFlashGBXManifest(QStringLiteral("loaded"));

		if (autoUploadEnabled && result->saveUploadArmed) {
			configureFlashGBXSaveWatcher();
		} else {
			m_flashgbxSaveWatcher.removePaths(m_flashgbxSaveWatcher.files());
			m_flashgbxSaveWatcher.removePaths(m_flashgbxSaveWatcher.directories());
		}

		m_flashgbxUseSoftwareDisplay = true;
		CoreController* controller = m_manager->loadGame(result->romPath, result->savePath);
		if (!controller) {
			m_flashgbxUseSoftwareDisplay = false;
			writeFlashGBXManifest(QStringLiteral("load-save-failed"));
			m_flashgbxSession.reset();
			showFlashGBXOverlayWarning(tr("Cartridge save data could not be loaded into the emulator. The ROM was not started."));
			return;
		}
		setController(controller);
		if (!result->saveUploadArmed) {
			showFlashGBXOverlayWarning(tr("ROM was loaded, but cartridge save data was not safely backed up. Save upload is disabled for this session, and the local save file will be kept."));
		}
	});
}

void Window::bootBIOS() {
	if (blockFlashGBXSaveUploadInProgress()) {
		return;
	}
	QString bios(m_config->getOption("gba.bios"));
	if (bios.isEmpty()) {
		bios = m_config->getOption("bios");
	}
	setController(m_manager->loadBIOS(mPLATFORM_GBA, bios));
}

#ifdef USE_SQLITE3
void Window::selectROMInArchive() {
	if (blockFlashGBXSaveUploadInProgress()) {
		return;
	}
	QString filename = GBAApp::app()->getOpenFileName(this, tr("Select ROM"), getFiltersArchive());
	if (filename.isEmpty()) {
		return;
	}
	ArchiveInspector* archiveInspector = new ArchiveInspector(filename);
	connect(archiveInspector, &QDialog::accepted, [this,  archiveInspector]() {
		VFile* output = archiveInspector->selectedVFile();
		QPair<QString, QString> path = archiveInspector->selectedPath();
		if (output) {
			setController(m_manager->loadGame(output, path.second, path.first));
		}
		archiveInspector->close();
	});
	archiveInspector->setAttribute(Qt::WA_DeleteOnClose);
	archiveInspector->show();
}

void Window::addDirToLibrary() {
	QString filename = GBAApp::app()->getOpenDirectoryName(this, tr("Select folder"));
	if (filename.isEmpty()) {
		return;
	}
	m_libraryView->addDirectory(filename);
}
#endif

void Window::replaceROM() {
	if (blockFlashGBXCartridgeRestrictedAction()) {
		return;
	}
	QString filename = GBAApp::app()->getOpenFileName(this, tr("Select ROM"), romFilters());
	if (!filename.isEmpty()) {
		m_controller->replaceGame(filename);
	}
}

void Window::selectSave(bool temporary) {
	if (blockFlashGBXCartridgeRestrictedAction()) {
		return;
	}
	QStringList formats{"*.sav"};
	QString filter = tr("Save games (%1)").arg(formats.join(QChar(' ')));
	QString filename = GBAApp::app()->getOpenFileName(this, tr("Select save game"), filter);
	if (!filename.isEmpty()) {
		m_controller->loadSave(filename, temporary);
	}
}

void Window::selectState(bool load) {
	if (blockFlashGBXCartridgeRestrictedAction()) {
		return;
	}
	QStringList formats{"*.ss0", "*.ss1", "*.ss2", "*.ss3", "*.ss4", "*.ss5", "*.ss6", "*.ss7", "*.ss8", "*.ss9"};
	QString filter = tr("mGBA save state files (%1)").arg(formats.join(QChar(' ')));
	if (load) {
		QString filename = GBAApp::app()->getOpenFileName(this, tr("Select save state"), filter);
		if (!filename.isEmpty()) {
			m_controller->loadState(filename);
		}
	} else {
		QString filename = GBAApp::app()->getSaveFileName(this, tr("Select save state"), filter);
		if (!filename.isEmpty()) {
			m_controller->saveState(filename);
		}
	}
}

void Window::multiplayerChanged() {
	if (!m_controller) {
		return;
	}
	int attached = 1;
	MultiplayerController* multiplayer = m_controller->multiplayerController();
	if (multiplayer) {
		attached = multiplayer->attached();
		m_playerId = multiplayer->playerId(m_controller.get());
	}
	for (auto& action : m_nonMpActions) {
		action->setEnabled(attached < 2);
	}
	updateFlashGBXRestrictedActions();
}

void Window::selectPatch() {
	if (blockFlashGBXCartridgeRestrictedAction()) {
		return;
	}
	QString filename = GBAApp::app()->getOpenFileName(this, tr("Select patch"), tr("Patches (*.ips *.ups *.bps)"));
	if (!filename.isEmpty()) {
		if (m_controller) {
			m_controller->loadPatch(filename);
		} else {
			m_pendingPatch = filename;
		}
	}
}

void Window::scanCard() {
	QStringList filenames = GBAApp::app()->getOpenFileNames(this, tr("Select e-Reader dotcode"), tr("e-Reader card (*.raw *.bin *.bmp)"));
	for (QString& filename : filenames) {
		m_controller->scanCard(filename);
	}
}

void Window::parseCard() {
#ifdef USE_FFMPEG
	QStringList filenames = GBAApp::app()->getOpenFileNames(this, tr("Select e-Reader card images"), tr("Image file (*.png *.jpg *.jpeg)"));
	QMessageBox* dialog = new QMessageBox(QMessageBox::Information, tr("Conversion finished"),
	                                      QString("oh"), QMessageBox::Ok);
	dialog->setAttribute(Qt::WA_DeleteOnClose);
	auto status = std::make_shared<QPair<int, int>>(0, filenames.size());
	GBAApp::app()->submitWorkerJob([filenames, status]() {
		int success = 0;
		for (QString filename : filenames) {
			if (filename.isEmpty()) {
				continue;
			}
			QImage image(filename);
			if (image.isNull()) {
				continue;
			}
			EReaderScan* scan;
			switch (image.depth()) {
			case 8:
				scan = EReaderScanLoadImage8(image.constBits(), image.width(), image.height(), image.bytesPerLine());
				break;
			case 24:
				scan = EReaderScanLoadImage(image.constBits(), image.width(), image.height(), image.bytesPerLine());
				break;
			case 32:
				scan = EReaderScanLoadImageA(image.constBits(), image.width(), image.height(), image.bytesPerLine());
				break;
			default:
				continue;
			}
			QFileInfo ofile(filename);
			if (EReaderScanCard(scan)) {
				QString ofilename = ofile.path() + QDir::separator() + ofile.baseName() + ".raw";
				EReaderScanSaveRaw(scan, ofilename.toUtf8().constData(), false);
				++success;
			}
			EReaderScanDestroy(scan);
		}
		status->first = success;
	}, [dialog, status]() {
		if (status->second == 0) {
			return;
		}
		dialog->setText(tr("%1 of %2 e-Reader cards converted successfully.").arg(status->first).arg(status->second));
		dialog->show();
	});
#endif
}

void Window::configureFlashGBXSaveWatcher() {
	if (!m_flashgbxSaveWatcher.files().isEmpty()) {
		m_flashgbxSaveWatcher.removePaths(m_flashgbxSaveWatcher.files());
	}
	if (!m_flashgbxSaveWatcher.directories().isEmpty()) {
		m_flashgbxSaveWatcher.removePaths(m_flashgbxSaveWatcher.directories());
	}
	if (!m_flashgbxSession) {
		return;
	}
	if (QFileInfo::exists(m_flashgbxSession->savePath)) {
		m_flashgbxSaveWatcher.addPath(m_flashgbxSession->savePath);
	}
}

void Window::scheduleFlashGBXSaveUpload() {
	if (!m_flashgbxSession) {
		return;
	}
	if (!m_flashgbxSession->saveUploadArmed) {
		return;
	}
	if (!m_config->getQtOption(QStringLiteral("autoUpload"), QStringLiteral("flashgbx")).toBool()) {
		return;
	}
	const QString saveHash = fileSha256Prefix(m_flashgbxSession->savePath, m_flashgbxSession->savePayloadSize);
	if (saveHash.isEmpty() || saveHash == m_flashgbxSession->initialSaveHash ||
	    saveHash == m_flashgbxUploadingSaveHash || saveHash == m_flashgbxQueuedSaveHash) {
		return;
	}
	m_flashgbxQueuedSaveHash = saveHash;
	m_flashgbxSaveUploadTimer.start();
}

bool Window::uploadFlashGBXSave(bool force) {
	if (!m_flashgbxSession) {
		if (force) {
			showFlashGBXOverlayWarning(tr("Load a cartridge first."));
		}
		return false;
	}
	if (!m_flashgbxSession->saveUploadArmed) {
		if (force) {
			showFlashGBXOverlayWarning(tr("Cartridge save data was not safely backed up, so mGBA will not write this session's save data back to the cartridge. The local save file was kept:\n%1")
			                           .arg(m_flashgbxSession->savePath));
		}
		return false;
	}
	if (!QFileInfo::exists(m_flashgbxSession->savePath)) {
		if (force) {
			showFlashGBXOverlayWarning(tr("No save file exists yet for this cartridge session."));
		}
		return false;
	}

	const QString saveHash = fileSha256Prefix(m_flashgbxSession->savePath, m_flashgbxSession->savePayloadSize);
	if (saveHash.isEmpty()) {
		if (force) {
			showFlashGBXOverlayWarning(tr("Could not read the save file."));
		}
		return false;
	}
	if (!force && saveHash == m_flashgbxSession->initialSaveHash) {
		if (m_flashgbxQueuedSaveHash == saveHash) {
			m_flashgbxQueuedSaveHash.clear();
		}
		return false;
	}
	if (!force && saveHash == m_flashgbxUploadingSaveHash) {
		return false;
	}
	if (m_flashgbxBusy) {
		m_flashgbxUploadPending = true;
		if (!force) {
			m_flashgbxQueuedSaveHash = saveHash;
		}
		return true;
	}

	m_flashgbxUploadingSaveHash = saveHash;
	m_flashgbxQueuedSaveHash.clear();
	m_flashgbxBusy = true;
	m_flashgbxSaveUploadBusy = true;
	updateFlashGBXSaveUploadActions();
	updateTitle();
	auto result = std::make_shared<FlashGBXUploadResult>();
	QStringList restoreCommand = m_flashgbxSession->restoreCommand;
	const QString verifyPath = QDir(m_flashgbxSession->sessionDir).filePath(QStringLiteral("cart.verify.sav"));
	const QString uploadPath = QDir(m_flashgbxSession->sessionDir).filePath(QStringLiteral("cart.upload.sav"));
	if (!restoreCommand.isEmpty()) {
		restoreCommand.last() = uploadPath;
	}
	const QString savePath = m_flashgbxSession->savePath;
	const qint64 savePayloadSize = m_flashgbxSession->savePayloadSize;
	GBAApp::app()->submitWorkerJob([result, restoreCommand, savePath, savePayloadSize, saveHash, verifyPath, uploadPath]() {
		QFile::remove(uploadPath);
		if (!copyFilePrefix(savePath, uploadPath, savePayloadSize)) {
			QFile::remove(uploadPath);
			result->error = QObject::tr("Could not read the save file.");
			result->output = QString();
			return;
		}
		FlashGBXProcessResult restoreResult = runProcess(restoreCommand);
		QString combinedOutput;
		appendProcessOutput(&combinedOutput, QStringLiteral("restore-save"), restoreResult);
		result->saveHash = saveHash;
		result->verifyPath = verifyPath;
		result->success = flashGBXActionSucceeded(QStringLiteral("restore-save"), restoreResult, restoreCommand.last());
		if (!result->success) {
			QFile::remove(uploadPath);
			result->error = QObject::tr("Could not restore the save data to the cartridge.");
			result->output = combinedOutput;
			return;
		}

		QFile::remove(verifyPath);
		QStringList verifyCommand = restoreCommand;
		for (int i = 0; i + 1 < verifyCommand.size(); ++i) {
			if (verifyCommand.at(i) == QLatin1String("--action")) {
				verifyCommand[i + 1] = QStringLiteral("backup-save");
				break;
			}
		}
		if (!verifyCommand.isEmpty()) {
			verifyCommand.last() = verifyPath;
		}

		FlashGBXProcessResult verifyResult = runProcess(verifyCommand);
		appendProcessOutput(&combinedOutput, QStringLiteral("verify-save"), verifyResult);
		result->verifyHash = fileSha256(verifyPath);
		result->success = flashGBXActionSucceeded(QStringLiteral("backup-save"), verifyResult, verifyPath) && result->verifyHash == saveHash;
		if (!result->success) {
			if (!combinedOutput.isEmpty()) {
				combinedOutput.append(QLatin1Char('\n'));
			}
			combinedOutput.append(QObject::tr("Expected save SHA-256: %1\nRead-back save SHA-256: %2")
			                      .arg(saveHash, result->verifyHash.isEmpty() ? QObject::tr("(unreadable)") : result->verifyHash));
			result->error = QObject::tr("The save data was written, but reading it back from the cartridge did not match. The cartridge save was not marked as restored.");
		} else {
			QFile::remove(verifyPath);
		}
		QFile::remove(uploadPath);
		result->output = combinedOutput;
	}, this, [this, result, force]() {
		m_flashgbxBusy = false;
		m_flashgbxSaveUploadBusy = false;
		updateFlashGBXSaveUploadActions();
		updateTitle();
		m_flashgbxUploadingSaveHash.clear();
		if (!m_flashgbxSession) {
			m_flashgbxQueuedSaveHash.clear();
			return;
		}
		if (result->success) {
			m_flashgbxSession->initialSaveHash = result->saveHash;
			if (m_flashgbxQueuedSaveHash == result->saveHash) {
				m_flashgbxQueuedSaveHash.clear();
			}
			writeFlashGBXManifest(QStringLiteral("restored"));
			if (!isFlashGBXCartridgeActive()) {
				cleanupFlashGBXSessionTempFiles(true);
			}
			if (force) {
				showFlashGBXOverlayWarning(tr("Save data was uploaded to the cartridge."));
			}
			if (m_flashgbxCloseAfterUpload) {
				m_flashgbxCloseAfterUpload = false;
				QTimer::singleShot(0, this, &Window::close);
				return;
			}
		} else {
			if (m_flashgbxQueuedSaveHash == result->saveHash) {
				m_flashgbxQueuedSaveHash.clear();
			}
			writeFlashGBXManifest(QStringLiteral("restore-failed"));
			showFlashGBXOverlayWarning(tr("%1\n\nThe save file was kept for manual restore:\n%2")
			                           .arg(result->error.isEmpty() ? tr("Could not restore the save data to the cartridge.") : result->error,
			                                m_flashgbxSession->savePath));
			if (m_flashgbxCloseAfterUpload) {
				m_flashgbxCloseAfterUpload = false;
				m_pendingClose = false;
			}
		}
		configureFlashGBXSaveWatcher();
		if (m_flashgbxUploadPending) {
			m_flashgbxUploadPending = false;
			scheduleFlashGBXSaveUpload();
		}
	});
	return true;
}

void Window::restoreFlashGBXSave() {
	uploadFlashGBXSave(true);
}

void Window::writeFlashGBXManifest(const QString& restoreStatus) {
	if (!m_flashgbxSession) {
		return;
	}

	QJsonObject manifest;
	manifest.insert(QStringLiteral("mode"), m_flashgbxSession->mode);
	manifest.insert(QStringLiteral("flashcart_type"), m_flashgbxSession->flashcartType);
	manifest.insert(QStringLiteral("save_type"), m_flashgbxSession->saveType);
	manifest.insert(QStringLiteral("dmg_mbc"), m_flashgbxSession->dmgMbc);
	manifest.insert(QStringLiteral("preload_warning"), m_flashgbxSession->preloadWarning);
	manifest.insert(QStringLiteral("rom_path"), m_flashgbxSession->romPath);
	manifest.insert(QStringLiteral("save_path"), m_flashgbxSession->savePath);
	manifest.insert(QStringLiteral("initial_save_path"), QFileInfo::exists(m_flashgbxSession->initialSavePath) ? m_flashgbxSession->initialSavePath : QString());
	manifest.insert(QStringLiteral("save_upload_armed"), m_flashgbxSession->saveUploadArmed);
	manifest.insert(QStringLiteral("initial_save_sha256"), m_flashgbxSession->initialSaveHash);
	manifest.insert(QStringLiteral("save_payload_size"), m_flashgbxSession->savePayloadSize);
	manifest.insert(QStringLiteral("restore_status"), restoreStatus);

	QFile manifestFile(QDir(m_flashgbxSession->sessionDir).filePath(QStringLiteral("cartridge-session.json")));
	if (manifestFile.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
		manifestFile.write(QJsonDocument(manifest).toJson(QJsonDocument::Indented));
	}
}

void Window::cleanupFlashGBXSessionTempFiles(bool removeSave) {
	if (!m_flashgbxSession) {
		return;
	}
	QFile::remove(m_flashgbxSession->romPath);
	QFile::remove(m_flashgbxSession->initialSavePath);
	QFile::remove(QDir(m_flashgbxSession->sessionDir).filePath(QStringLiteral("cart.verify.sav")));
	QFile::remove(QDir(m_flashgbxSession->sessionDir).filePath(QStringLiteral("cart.upload.sav")));
	if (removeSave) {
		QFile::remove(m_flashgbxSession->savePath);
	}
}

bool Window::isFlashGBXCartridgeActive() const {
	if (!m_controller || !m_flashgbxSession) {
		return false;
	}

	QFileInfo expectedInfo(m_flashgbxSession->romPath);
	QString expectedPath = expectedInfo.canonicalFilePath();
	if (expectedPath.isEmpty()) {
		expectedPath = expectedInfo.absoluteFilePath();
	}

	const QString controllerPath = m_controller->path();
	const QString controllerBase = m_controller->baseDirectory();
	QFileInfo currentInfo(controllerBase.isEmpty() ? controllerPath : QDir(controllerBase).filePath(controllerPath));
	QString currentPath = currentInfo.canonicalFilePath();
	if (currentPath.isEmpty()) {
		currentPath = currentInfo.absoluteFilePath();
	}

	return !expectedPath.isEmpty() && expectedPath == currentPath;
}

bool Window::blockFlashGBXCartridgeRestrictedAction() {
	if (!isFlashGBXCartridgeActive()) {
		return false;
	}
	showFlashGBXOverlayWarning(tr("This feature is disabled while a cartridge is loaded."));
	return true;
}

bool Window::blockFlashGBXSaveUploadInProgress() {
	if (!m_flashgbxSaveUploadBusy) {
		return false;
	}
	showFlashGBXOverlayWarning(tr("Save data is being written to the cartridge. Wait until it finishes before resetting or closing mGBA."));
	return true;
}

void Window::showFlashGBXOverlayWarning(const QString& message) {
	if (message.isEmpty()) {
		return;
	}

	if (m_display) {
		m_display->showMessage(message);
	}

	QWidget* anchor = centralWidget() ? centralWidget() : this;
	QRect anchorRect = anchor == this ? rect() : anchor->geometry();
	if (!anchorRect.isValid()) {
		anchorRect = rect();
	}

	QLabel* overlay = new QLabel(message, this);
	overlay->setObjectName(QStringLiteral("flashgbxOverlayWarning"));
	overlay->setAttribute(Qt::WA_DeleteOnClose);
	overlay->setAttribute(Qt::WA_TransparentForMouseEvents);
	overlay->setAlignment(Qt::AlignCenter);
	overlay->setWordWrap(true);
	overlay->setStyleSheet(QStringLiteral(
	    "QLabel#flashgbxOverlayWarning {"
	    "background-color: rgba(20, 22, 26, 230);"
	    "color: white;"
	    "border: 1px solid rgba(255, 255, 255, 90);"
	    "border-radius: 6px;"
	    "padding: 10px 14px;"
	    "}"));

	const int maxWidth = qMax(120, anchorRect.width() - 24);
	const int minWidth = qMin(maxWidth, 280);
	overlay->setMaximumWidth(maxWidth);
	overlay->adjustSize();
	const QSize hint = overlay->sizeHint();
	const int overlayWidth = qBound(minWidth, hint.width(), maxWidth);
	overlay->setFixedWidth(overlayWidth);
	overlay->adjustSize();
	const int maxHeight = qMax(64, anchorRect.height() - 32);
	const int overlayHeight = qMin(overlay->sizeHint().height(), maxHeight);
	const int overlayX = qMax(anchorRect.x() + 12, anchorRect.x() + (anchorRect.width() - overlayWidth) / 2);
	const int overlayY = anchorRect.y() + qMax(16, anchorRect.height() / 12);
	overlay->setGeometry(overlayX, overlayY, overlayWidth, overlayHeight);
	overlay->raise();
	overlay->show();
	QTimer::singleShot(6500, overlay, &QLabel::close);
}

bool Window::confirmFlashGBXOverlayWarning(const QString& message) {
	if (message.isEmpty()) {
		return true;
	}

	if (m_display) {
		m_display->showMessage(message);
	}

	QWidget* anchor = centralWidget() ? centralWidget() : this;
	QRect anchorRect = anchor == this ? rect() : anchor->rect();
	if (!anchorRect.isValid()) {
		anchorRect = rect();
	}

	QDialog dialog(this);
	dialog.setObjectName(QStringLiteral("flashgbxOverlayPrompt"));
	dialog.setWindowFlags(Qt::Dialog | Qt::FramelessWindowHint);
	dialog.setWindowModality(Qt::WindowModal);
	dialog.setStyleSheet(QStringLiteral(
	    "QDialog#flashgbxOverlayPrompt {"
	    "background-color: rgba(20, 22, 26, 245);"
	    "color: white;"
	    "border: 1px solid rgba(255, 255, 255, 110);"
	    "border-radius: 6px;"
	    "}"
	    "QLabel { color: white; }"
	    "QPushButton { padding: 6px 14px; min-width: 84px; }"));

	QVBoxLayout layout(&dialog);
	layout.setContentsMargins(18, 16, 18, 14);
	layout.setSpacing(12);
	QScrollArea scroll(&dialog);
	scroll.setWidgetResizable(true);
	scroll.setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
	scroll.setStyleSheet(QStringLiteral("QScrollArea { border: none; background: transparent; }"));
	QWidget* messageWidget = new QWidget(&scroll);
	QVBoxLayout* messageLayout = new QVBoxLayout(messageWidget);
	messageLayout->setContentsMargins(0, 0, 0, 0);
	QLabel* label = new QLabel(message, messageWidget);
	label->setWordWrap(true);
	label->setAlignment(Qt::AlignCenter);
	messageLayout->addWidget(label);
	scroll.setWidget(messageWidget);
	layout.addWidget(&scroll);

	QDialogButtonBox buttons(&dialog);
	QPushButton* loadButton = buttons.addButton(tr("Load Anyway"), QDialogButtonBox::AcceptRole);
	QPushButton* cancelButton = buttons.addButton(tr("Cancel"), QDialogButtonBox::RejectRole);
	loadButton->setDefault(true);
	cancelButton->setAutoDefault(false);
	layout.addWidget(&buttons);

	connect(&buttons, &QDialogButtonBox::accepted, &dialog, &QDialog::accept);
	connect(&buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);

	QRect availableRect = screen() ? screen()->availableGeometry() : QRect();
	if (!availableRect.isValid()) {
		availableRect = QRect(QPoint(0, 0), QSize(qMax(640, anchorRect.width()), qMax(480, anchorRect.height())));
	}
	const int maxWidth = qMax(360, qMin(anchorRect.width() - 32, availableRect.width() - 80));
	const int promptWidth = qBound(360, maxWidth, 720);
	scroll.setMaximumHeight(qMax(180, qMin(availableRect.height() - 180, 460)));
	dialog.setFixedWidth(promptWidth);
	dialog.adjustSize();
	const QPoint center = anchor->mapToGlobal(anchorRect.center());
	dialog.move(center.x() - dialog.width() / 2, center.y() - dialog.height() / 2);
	return dialog.exec() == QDialog::Accepted;
}

void Window::loadFlashGBXSafeState(int slot) {
	if (!m_controller) {
		return;
	}
	if (!isFlashGBXCartridgeActive()) {
		m_controller->loadState(slot);
		return;
	}

	int flags = m_config->getOption(QStringLiteral("loadStateExtdata"), SAVESTATE_SCREENSHOT | SAVESTATE_RTC).toInt();
	flags &= ~SAVESTATE_SAVEDATA;
	m_controller->loadState(slot, flags);
}

void Window::restrictFlashGBXAction(const std::shared_ptr<Action>& action) {
	if (!action) {
		return;
	}
	m_flashgbxRestrictedActions.append(action);
	updateFlashGBXRestrictedActions();
}

void Window::updateFlashGBXRestrictedActions() {
	const bool active = isFlashGBXCartridgeActive();
	if (!active && !m_flashgbxRestrictionsActive) {
		return;
	}

	if (!active) {
		for (auto iter = m_flashgbxRestrictedActionStates.begin(); iter != m_flashgbxRestrictedActionStates.end(); ++iter) {
			if (iter.key()) {
				iter.key()->setEnabled(iter.value());
			}
		}
		m_flashgbxRestrictedActionStates.clear();
		m_flashgbxRestrictionsActive = false;
		return;
	}

	m_flashgbxRestrictionsActive = true;
	for (auto& action : m_flashgbxRestrictedActions) {
		if (!action) {
			continue;
		}
		Action* rawAction = action.get();
		if (!m_flashgbxRestrictedActionStates.contains(rawAction)) {
			m_flashgbxRestrictedActionStates.insert(rawAction, rawAction->isEnabled());
		}
		rawAction->setEnabled(false);
	}
}

void Window::blockFlashGBXSaveUploadAction(const std::shared_ptr<Action>& action) {
	if (!action) {
		return;
	}
	m_flashgbxSaveUploadBlockedActions.append(action);
	updateFlashGBXSaveUploadActions();
}

void Window::updateFlashGBXSaveUploadActions() {
	if (!m_flashgbxSaveUploadBusy && !m_flashgbxSaveUploadActionsBlocked) {
		return;
	}

	if (!m_flashgbxSaveUploadBusy) {
		for (auto iter = m_flashgbxSaveUploadActionStates.begin(); iter != m_flashgbxSaveUploadActionStates.end(); ++iter) {
			if (iter.key()) {
				iter.key()->setEnabled(iter.value());
			}
		}
		m_flashgbxSaveUploadActionStates.clear();
		m_flashgbxSaveUploadActionsBlocked = false;
		updateFlashGBXRestrictedActions();
		return;
	}

	m_flashgbxSaveUploadActionsBlocked = true;
	for (auto& action : m_flashgbxSaveUploadBlockedActions) {
		if (!action) {
			continue;
		}
		Action* rawAction = action.get();
		if (!m_flashgbxSaveUploadActionStates.contains(rawAction)) {
			m_flashgbxSaveUploadActionStates.insert(rawAction, rawAction->isEnabled());
		}
		rawAction->setEnabled(false);
	}
}

void Window::openView(QWidget* widget) {
	connect(this, &Window::shutdown, widget, &QWidget::close);
	widget->setAttribute(Qt::WA_DeleteOnClose);
	widget->show();
}

void Window::showMenu(bool show) {
	if (auto hideMenu = m_actions.getAction("hideMenu")) {
		hideMenu->setActive(!show);
	}
	menuBar()->setVisible(show);
}

void Window::loadCamImage() {
	QString filename = GBAApp::app()->getOpenFileName(this, tr("Select image"), tr("Image file (*.png *.gif *.jpg *.jpeg);;All files (*)"));
	if (!filename.isEmpty()) {
		m_inputController.loadCamImage(filename);
	}
}

void Window::importSharkport() {
	if (blockFlashGBXCartridgeRestrictedAction()) {
		return;
	}
	QString filename = GBAApp::app()->getOpenFileName(this, tr("Select save"), tr("GameShark saves (*.gsv *.sps *.xps)"));
	if (!filename.isEmpty()) {
		m_controller->importSharkport(filename);
	}
}

void Window::exportSharkport() {
	QString filename = GBAApp::app()->getSaveFileName(this, tr("Select save"), tr("GameShark saves (*.sps *.xps)"));
	if (!filename.isEmpty()) {
		m_controller->exportSharkport(filename);
	}
}

void Window::openSettingsWindow() {
	openSettingsWindow(SettingsView::Page::AV);
}

void Window::openSettingsWindow(SettingsView::Page page) {
	SettingsView* settingsWindow = new SettingsView(m_config, &m_inputController, m_shortcutController, &m_log);
#if defined(BUILD_GL) || defined(BUILD_GLES2)
	if (m_display->supportsShaders()) {
		settingsWindow->setShaderSelector(m_shaderView.get());
	}
#endif
	connect(settingsWindow, &SettingsView::displayDriverChanged, this, &Window::reloadDisplayDriver);
	connect(settingsWindow, &SettingsView::audioDriverChanged, this, &Window::reloadAudioDriver);
	connect(settingsWindow, &SettingsView::cameraDriverChanged, this, &Window::mustReset);
	connect(settingsWindow, &SettingsView::cameraChanged, &m_inputController, &InputController::setCamera);
	connect(settingsWindow, &SettingsView::videoRendererChanged, this, &Window::changeRenderer);
	connect(settingsWindow, &SettingsView::languageChanged, this, &Window::mustRestart);
	connect(settingsWindow, &SettingsView::pathsChanged, this, &Window::reloadConfig);
#ifdef USE_SQLITE3
	connect(settingsWindow, &SettingsView::libraryCleared, m_libraryView, &LibraryController::clear);
#endif
#ifdef ENABLE_SCRIPTING
	connect(settingsWindow, &SettingsView::openAutorunScripts, this, [this]() {
		ensureScripting();
		m_scripting->openAutorunEdit();
	});
#endif
	connect(this, &Window::shaderSelectorAdded, settingsWindow, &SettingsView::setShaderSelector);
	openView(settingsWindow);
	settingsWindow->selectPage(page);
}

void Window::startVideoLog() {
	QString filename = GBAApp::app()->getSaveFileName(this, tr("Select video log"), tr("Video logs (*.mvl)"));
	if (!filename.isEmpty()) {
		m_controller->startVideoLog(filename);
	}
}

template <typename T, typename... A>
std::function<void()> Window::openTView(A... arg) {
	return [=]() {
		T* view = new T(arg...);
		openView(view);
	};
}

template <typename T, typename... A>
std::function<void()> Window::openControllerTView(A... arg) {
	return [=]() {
		T* view = new T(m_controller, arg...);
		connect(m_controller.get(), &CoreController::stopping, view, &QWidget::close);
		openView(view);
	};
}

template <typename T, typename... A>
std::function<void()> Window::openNamedTView(QPointer<T>* name, bool keepalive, A... arg) {
	return [=]() {
		if (!*name) {
			*name = new T(arg...);
			connect(this, &Window::shutdown, name->data(), &QWidget::close);
			if (!keepalive) {
				(*name)->setAttribute(Qt::WA_DeleteOnClose);
			}
		}
		(*name)->show();
		(*name)->activateWindow();
		(*name)->raise();
	};
}

template <typename T, typename... A>
std::function<void()> Window::openNamedControllerTView(QPointer<T>* name, bool keepalive, A... arg) {
	return [=]() {
		if (!*name) {
			*name = new T(m_controller, arg...);
			connect(m_controller.get(), &CoreController::stopping, name->data(), &QWidget::close);
			connect(this, &Window::shutdown, name->data(), &QWidget::close);
			if (!keepalive) {
				(*name)->setAttribute(Qt::WA_DeleteOnClose);
			}
		}
		(*name)->show();
		(*name)->activateWindow();
		(*name)->raise();
	};
}

#ifdef ENABLE_GDB_STUB
void Window::gdbOpen() {
	if (!m_gdbController) {
		m_gdbController = new GDBController(this);
	}
	GDBWindow* window = new GDBWindow(m_gdbController);
	m_gdbController->setController(m_controller);
	connect(m_controller.get(), &CoreController::stopping, window, &QWidget::close);
	openView(window);
}
#endif

#ifdef ENABLE_DEBUGGERS
void Window::consoleOpen() {
	if (!m_console) {
		m_console = new DebuggerConsoleController(this);
	}
	DebuggerConsole* window = new DebuggerConsole(m_console);
	if (m_controller) {
		m_console->setController(m_controller);
	}
	openView(window);
}
#endif

#ifdef ENABLE_SCRIPTING
void Window::scriptingOpen() {
	ensureScripting();
	ScriptingView* view = new ScriptingView(m_scripting.get(), m_config);
	openView(view);
}
#endif

void Window::keyPressEvent(QKeyEvent* event) {
	if (event->isAutoRepeat()) {
		QWidget::keyPressEvent(event);
		return;
	}
	int key = m_inputController.mapKeyboard(event->key());
	if (key == -1) {
		QWidget::keyPressEvent(event);
		return;
	}
	if (m_controller) {
		m_controller->addKey(key);
	}
	event->accept();
}

void Window::keyReleaseEvent(QKeyEvent* event) {
	if (event->isAutoRepeat()) {
		QWidget::keyReleaseEvent(event);
		return;
	}
	int key = m_inputController.mapKeyboard(event->key());
	if (key == -1) {
		QWidget::keyPressEvent(event);
		return;
	}
	if (m_controller) {
		m_controller->clearKey(key);
	}
	event->accept();
}

void Window::resizeEvent(QResizeEvent*) {
	QSize newSize = centralWidget()->size();
	if (!isFullScreen()) {
		m_config->setOption("height", newSize.height());
		m_config->setOption("width", newSize.width());
	}

	int factor = 0;
	QSize size(GBA_VIDEO_HORIZONTAL_PIXELS, GBA_VIDEO_VERTICAL_PIXELS);
	if (m_controller) {
		size = m_controller->screenDimensions();
	}
	if (newSize.width() % size.width() == 0 && newSize.height() % size.height() == 0 &&
	    newSize.width() / size.width() == newSize.height() / size.height()) {
		factor = newSize.width() / size.width();
	}
	m_savedScale = factor;
	for (QMap<int, std::shared_ptr<Action>>::iterator iter = m_frameSizes.begin(); iter != m_frameSizes.end(); ++iter) {
		iter.value()->setActive(iter.key() == factor);
	}

	m_config->setOption("fullscreen", isFullScreen());
}

void Window::showEvent(QShowEvent* event) {
	if (m_wasOpened) {
		if (event->spontaneous() && m_controller) {
			focusCheck();
			if (m_config->getOption("pauseOnMinimize").toInt() && m_autoresume) {
				m_controller->setPaused(false);
				m_autoresume = false;
			}

			if (m_config->getOption("muteOnMinimize").toInt()) {
				m_minimizedMute = false;
				updateMute();
			}
		}
		return;
	}
	m_wasOpened = true;
#ifdef Q_OS_WIN
	HWND hwnd = reinterpret_cast<HWND>(winId());
	DWM_WINDOW_CORNER_PREFERENCE cornerPref = DWMWCP_DONOTROUND;
	DwmSetWindowAttribute(hwnd, DWMWA_WINDOW_CORNER_PREFERENCE, &cornerPref, sizeof(cornerPref));
#endif
	if (m_initialSize.isValid()) {
		resizeFrame(m_initialSize);
	}
	QVariant windowPos = m_config->getQtOption("windowPos", m_playerId > 0 ? QString("player%0").arg(m_playerId) : QString());
	bool maximized = m_config->getQtOption("maximized").toBool();
	QRect geom = windowHandle()->screen()->availableGeometry();
	if (!windowPos.isNull() && geom.contains(windowPos.toPoint())) {
		move(windowPos.toPoint());
	} else {
		QRect rect = frameGeometry();
		rect.moveCenter(geom.center());
		move(rect.topLeft());
	}
	if (maximized) {
		showMaximized();
	}
	if (m_fullscreenOnStart) {
		enterFullScreen();
		m_fullscreenOnStart = false;
	}
	reloadDisplayDriver();
	setFocus();
}

void Window::hideEvent(QHideEvent* event) {
	if (!event->spontaneous()) {
		return;
	}
	if (!m_controller) {
		return;
	}

	if (m_config->getOption("pauseOnMinimize").toInt() && !m_controller->isPaused()) {
		m_autoresume = true;
		m_controller->setPaused(true);
	}
	if (m_config->getOption("muteOnMinimize").toInt()) {
		m_minimizedMute = true;
		updateMute();
	}
}

void Window::closeEvent(QCloseEvent* event) {
	if (blockFlashGBXSaveUploadInProgress()) {
		event->ignore();
		return;
	}
	emit shutdown();
	m_config->setQtOption("windowPos", pos(), m_playerId > 0 ? QString("player%0").arg(m_playerId) : QString());
	m_config->setQtOption("maximized", isMaximized());

	if (m_savedScale > 0) {
		m_config->setOption("height", GBA_VIDEO_VERTICAL_PIXELS * m_savedScale);
		m_config->setOption("width", GBA_VIDEO_HORIZONTAL_PIXELS * m_savedScale);
	}
	saveConfig();
	if (m_controller) {
		event->ignore();
		m_pendingClose = true;
	} else {
		m_display.reset();
	}
}

void Window::focusInEvent(QFocusEvent*) {
	for (Window* window : GBAApp::app()->windows()) {
		if (window != this) {
			window->updateMultiplayerActive(false);
		} else {
			updateMultiplayerActive(true);
		}
	}
	if (m_display) {
		m_display->forceDraw();
	}
}

void Window::focusOutEvent(QFocusEvent*) {
}

void Window::dragEnterEvent(QDragEnterEvent* event) {
	if (event->mimeData()->hasFormat("text/uri-list")) {
		event->acceptProposedAction();
	}
}

void Window::dropEvent(QDropEvent* event) {
	QString uris = event->mimeData()->data("text/uri-list");
	uris = uris.trimmed();
	if (uris.contains("\n")) {
		// Only one file please
		return;
	}
	QUrl url(uris);
	if (!url.isLocalFile()) {
		// No remote loading
		return;
	}
	event->accept();
	setController(m_manager->loadGame(url.toLocalFile()));
}

#ifndef Q_OS_MAC
void Window::changeEvent(QEvent* event) {
	if (event->type() == QEvent::WindowStateChange) {
		if (isFullScreen()) {
			if (m_controller && !m_controller->isPaused()) {
				showMenu(false);
			} else {
				showMenu(true);
			}
		}
	}
}
#endif

void Window::enterFullScreen() {
	if (!isVisible()) {
		m_fullscreenOnStart = true;
		return;
	}
	if (isFullScreen()) {
		return;
	}
	showFullScreen();
#ifndef Q_OS_MAC
	if (m_controller && !m_controller->isPaused()) {
		showMenu(false);
	}
#endif
}

void Window::exitFullScreen() {
	showMenu(true);
	if (!isFullScreen()) {
		return;
	}
	centralWidget()->unsetCursor();
	showNormal();
}

void Window::toggleFullScreen() {
	if (isFullScreen()) {
		exitFullScreen();
	} else {
		enterFullScreen();
	}
}

void Window::gameStarted() {
	for (auto& action : m_gameActions) {
		action->setEnabled(true);
	}
	for (auto action = m_platformActions.begin(); action != m_platformActions.end(); ++action) {
		action.value()->setEnabled(m_controller->platform() == action.key());
	}
	QSize size = m_controller->screenDimensions();
	m_config->updateOption("lockIntegerScaling");
	m_config->updateOption("lockAspectRatio");
	m_config->updateOption("interframeBlending");
	m_config->updateOption("resampleVideo");
	if (m_savedScale > 0) {
		resizeFrame(size * m_savedScale);
	}
	attachWidget(m_display.get());
	setFocus();

#ifndef Q_OS_MAC
	if (isFullScreen()) {
		showMenu(false);
	}
#endif

	reloadAudioDriver();
	multiplayerChanged();
	updateTitle();
	updateFlashGBXRestrictedActions();
	if (isFlashGBXCartridgeActive()) {
		cleanupFlashGBXSessionTempFiles();
	}

	m_hitUnimplementedBiosCall = false;
	if (m_config->getOption("showFps", "1").toInt()) {
		m_fpsTimer.start();
		m_frameTimer.start();
	}
	m_focusCheck.start();
	if (m_display->underMouse()) {
		centralWidget()->setCursor(Qt::BlankCursor);
	}

	CoreController::Interrupter interrupter(m_controller);
	mCore* core = m_controller->thread()->core;
	m_actions.clearMenu("videoLayers");
	m_actions.clearMenu("audioChannels");
	const mCoreChannelInfo* videoLayers;
	const mCoreChannelInfo* audioChannels;
	size_t nVideo = core->listVideoLayers(core, &videoLayers);
	size_t nAudio = core->listAudioChannels(core, &audioChannels);

	if (nVideo) {
		for (size_t i = 0; i < nVideo; ++i) {
			auto action = m_actions.addBooleanAction(videoLayers[i].visibleName, QString("videoLayer.%1").arg(videoLayers[i].internalName), [this, videoLayers, i](bool enable) {
				m_controller->thread()->core->enableVideoLayer(m_controller->thread()->core, videoLayers[i].id, enable);
			}, "videoLayers");
			action->setActive(true);
		}
	}
	if (nAudio) {
		for (size_t i = 0; i < nAudio; ++i) {
			auto action = m_actions.addBooleanAction(audioChannels[i].visibleName, QString("audioChannel.%1").arg(audioChannels[i].internalName), [this, audioChannels, i](bool enable) {
				m_controller->thread()->core->enableAudioChannel(m_controller->thread()->core, audioChannels[i].id, enable);
			}, "audioChannels");
			action->setActive(true);
		}
	}
	interrupter.resume();

	m_actions.rebuildMenu(menuBar(), this, *m_shortcutController);

#ifdef M_CORE_GBA
	if (m_controller->platform() == mPLATFORM_GBA) {
		QVariant eCardList = m_config->takeArgvOption(QString("ecard"));
#if (QT_VERSION < QT_VERSION_CHECK(6, 0, 0))
		if (eCardList.canConvert(QMetaType::QStringList)) {
#else
		if (QMetaType::canConvert(eCardList.metaType(), QMetaType(QMetaType::QStringList))) {
#endif
			m_controller->scanCards(eCardList.toStringList());
		}
	}
#endif

#ifdef USE_DISCORD_RPC
	DiscordCoordinator::gameStarted(m_controller);
#endif
}

void Window::gameStopped() {
	if (m_flashgbxSession) {
		if (m_pendingClose && m_config->getQtOption(QStringLiteral("autoUpload"), QStringLiteral("flashgbx")).toBool()) {
			m_flashgbxSaveUploadTimer.stop();
			m_flashgbxCloseAfterUpload = uploadFlashGBXSave(false);
		} else {
			scheduleFlashGBXSaveUpload();
		}
		cleanupFlashGBXSessionTempFiles();
	}

	for (auto& action : m_platformActions) {
		action->setEnabled(true);
	}
	for (auto& action : m_nonMpActions) {
		action->setEnabled(true);
	}
	for (auto& action : m_gameActions) {
		action->setEnabled(false);
		action->setActive(false);
	}
	setWindowFilePath(QString());

	m_actions.clearMenu("videoLayers");
	m_actions.clearMenu("audioChannels");

	m_fpsTimer.stop();
	m_focusCheck.stop();

	if (m_audioProcessor) {
		m_audioProcessor->stop();
		m_audioProcessor.reset();
	}
	m_display->stopDrawing();
	setLogo();
	if (m_display) {
#ifdef M_CORE_GB
		m_display->setMinimumSize(GB_VIDEO_HORIZONTAL_PIXELS, GB_VIDEO_VERTICAL_PIXELS);
#elif defined(M_CORE_GBA)
		m_display->setMinimumSize(GBA_VIDEO_HORIZONTAL_PIXELS, GBA_VIDEO_VERTICAL_PIXELS);
#endif
	}

	m_cleanupController = std::move(m_controller);
	detachWidget();
	updateTitle();

	if (m_pendingClose) {
#ifdef ENABLE_SCRIPTING
		std::shared_ptr<VideoProxy> proxy = m_display->videoProxy();
		if (m_scripting && proxy) {
			m_scripting->setVideoBackend(nullptr);
		}
#endif
		if (!m_flashgbxCloseAfterUpload) {
			m_cleanupDisplay = std::move(m_display);
			close();
		}
	}
	QTimer::singleShot(0, this, &Window::delayedCleanup);
#ifndef Q_OS_MAC
	showMenu(true);
#endif

#ifdef USE_DISCORD_RPC
	DiscordCoordinator::gameStopped();
#endif

	emit paused(false);
}

void Window::gameCrashed(const QString& errorMessage) {
	QMessageBox* crash = new QMessageBox(QMessageBox::Critical, tr("Crash"),
	                                     tr("The game has crashed with the following error:\n\n%1").arg(errorMessage),
	                                     QMessageBox::Ok, this, Qt::Sheet);
	crash->setAttribute(Qt::WA_DeleteOnClose);
	crash->show();
}

void Window::gameFailed() {
	cleanupFlashGBXSessionTempFiles();
	QMessageBox* fail = new QMessageBox(QMessageBox::Warning, tr("Couldn't Start"),
	                                    tr("Could not start game."),
	                                    QMessageBox::Ok, this, Qt::Sheet);
	fail->setAttribute(Qt::WA_DeleteOnClose);
	fail->show();
}

void Window::unimplementedBiosCall(int) {
	// TODO: Mention which call?
	if (m_hitUnimplementedBiosCall) {
		return;
	}
	m_hitUnimplementedBiosCall = true;

	QMessageBox* fail = new QMessageBox(
	    QMessageBox::Warning, tr("Unimplemented BIOS call"),
	    tr("This game uses a BIOS call that is not implemented. Please use the official BIOS for best experience."),
	    QMessageBox::Ok, this, Qt::Sheet);
	fail->setAttribute(Qt::WA_DeleteOnClose);
	fail->show();
}

void Window::reloadDisplayDriver() {
	if (m_controller) {
		m_display->stopDrawing();
		detachWidget();
	}
#ifdef ENABLE_SCRIPTING
	if (m_scripting) {
		m_scripting->setVideoBackend(nullptr);
	}
#endif
	std::shared_ptr<VideoProxy> proxy;
	if (m_display) {
		proxy = m_display->videoProxy();
	}
	m_display = std::unique_ptr<QGBA::Display>(Display::create(this));
	if (!m_display) {
		LOG(QT, ERROR) << tr("Failed to create an appropriate display device, falling back to software display. "
		                     "Games may run slowly, especially with larger windows.");
		Display::setDriver(Display::Driver::QT);
		m_display = std::unique_ptr<Display>(Display::create(this));
	}
#if defined(BUILD_GL) || defined(BUILD_GLES2)
	m_shaderView.reset();
	if (m_display->supportsShaders()) {
		m_shaderView = std::make_unique<ShaderSelector>(m_display.get(), m_config);
		emit shaderSelectorAdded(m_shaderView.get());
	} else {
		emit shaderSelectorAdded(nullptr);
	}
#endif

	connect(m_display.get(), &QGBA::Display::hideCursor, [this]() {
		if (centralWidget() == m_display.get()) {
			centralWidget()->setCursor(Qt::BlankCursor);
		}
	});
	connect(m_display.get(), &QGBA::Display::showCursor, [this]() {
		centralWidget()->unsetCursor();
	});

	m_display->configure(m_config);
#if defined(BUILD_GL) || defined(BUILD_GLES2)
	if (m_shaderView) {
		m_shaderView->refreshShaders();
	}
#endif

	if (m_controller) {
		attachDisplay();

		attachWidget(m_display.get());
	}
#ifdef M_CORE_GB
	m_display->setMinimumSize(GB_VIDEO_HORIZONTAL_PIXELS, GB_VIDEO_VERTICAL_PIXELS);
#elif defined(M_CORE_GBA)
	m_display->setMinimumSize(GBA_VIDEO_HORIZONTAL_PIXELS, GBA_VIDEO_VERTICAL_PIXELS);
#endif

	QString backgroundImage = m_config->getOption("backgroundImage");
	if (backgroundImage.isEmpty()) {
		m_display->setBackgroundImage(QImage{});
	} else {
		m_display->setBackgroundImage(QImage{backgroundImage});
	}

	if (!proxy) {
		proxy = std::make_shared<VideoProxy>();
	}
	m_display->setVideoProxy(std::move(proxy));
#ifdef ENABLE_SCRIPTING
	if (m_scripting) {
		m_scripting->setVideoBackend(m_display->videoBackend());
	}
#endif
}

void Window::reloadAudioDriver() {
	if (!m_controller) {
		return;
	}
	if (m_audioProcessor) {
		m_audioProcessor->stop();
		m_audioProcessor.reset();
	}

	m_audioProcessor = std::unique_ptr<AudioProcessor>(AudioProcessor::create());
	m_audioProcessor->setInput(m_controller);
	m_audioProcessor->configure(m_config);
	if (!m_audioProcessor->start()) {
		LOG(QT, WARN) << tr("Failed to start audio processor");
	}
}

void Window::changeRenderer() {
	if (!m_controller) {
		return;
	}

	CoreController::Interrupter interrupter(m_controller);
	if (m_config->getOption("hwaccelVideo").toInt() && m_display->supportsShaders() && m_controller->supportsFeature(CoreController::Feature::OPENGL)) {
		m_display->videoProxy()->attach(m_controller.get());

		int fb = m_display->framebufferHandle();
		if (fb >= 0) {
			m_controller->setFramebufferHandle(fb);
			m_config->updateOption("videoScale");
		}
	} else {
		m_display->videoProxy()->detach(m_controller.get());
		m_controller->setFramebufferHandle(-1);
	}
}

void Window::tryMakePortable() {
	QMessageBox* confirm = new QMessageBox(QMessageBox::Question, tr("Really make portable?"),
	                                       tr("This will make the emulator load its configuration from the same directory as the executable. Do you want to continue?"),
	                                       QMessageBox::Yes | QMessageBox::Cancel, this, Qt::Sheet);
	confirm->setAttribute(Qt::WA_DeleteOnClose);
	connect(confirm->button(QMessageBox::Yes), &QAbstractButton::clicked, m_config, &ConfigController::makePortable);
	confirm->show();
}

void Window::mustRestart() {
	if (m_mustRestart.isActive()) {
		return;
	}
	m_mustRestart.start();
	QMessageBox* dialog = new QMessageBox(QMessageBox::Warning, tr("Restart needed"),
	                                      tr("Some changes will not take effect until the emulator is restarted."),
	                                      QMessageBox::Ok, this, Qt::Sheet);
	dialog->setAttribute(Qt::WA_DeleteOnClose);
	dialog->show();
}

void Window::mustReset() {
	if (m_mustReset.isActive() || !m_controller) {
		return;
	}
	m_mustReset.start();
	QMessageBox* dialog = new QMessageBox(QMessageBox::Warning, tr("Reset needed"),
	                                      tr("Some changes will not take effect until the game is reset."),
	                                      QMessageBox::Ok, this, Qt::Sheet);
	dialog->setAttribute(Qt::WA_DeleteOnClose);
	dialog->show();
}

void Window::recordFrame() {
	m_frameList.append(m_frameTimer.nsecsElapsed());
	m_frameTimer.restart();
}

void Window::showFPS() {
	qint64 total = 0;
	for (qint64 t : m_frameList) {
		total += t;
	}
	if (!total) {
		updateTitle();
		return;
	}
	double fps = (m_frameList.size() * 1e10) / total;
	m_frameList.clear();
	fps = round(fps) / 10.f;
	updateTitle(fps);
}

void Window::updateTitle(float fps) {
	QString title;
	if (m_config->getOption("dynamicTitle", 1).toInt() && m_controller) {
		QString filePath = windowFilePath();
		if (m_config->getOption("showFilename").toInt() && !filePath.isNull()) {
			QFileInfo fileInfo(filePath);
			title = fileInfo.fileName();
		} else {
			title = m_controller->title();
		}

		MultiplayerController* multiplayer = m_controller->multiplayerController();
		if (multiplayer && multiplayer->attached() > 1) {
			title += tr(" -  Player %1 of %2").arg(m_playerId + 1).arg(multiplayer->attached());
			for (auto& action : m_nonMpActions) {
				action->setEnabled(false);
			}
		} else {
			for (auto& action : m_nonMpActions) {
				action->setEnabled(true);
			}
		}
	}
	if (title.isNull()) {
		setWindowTitle(tr("%1%2 - %3").arg(projectName).arg(m_flashgbxSaveUploadBusy ? tr(" - Saving to Cartridge") : QString()).arg(projectVersion));
	} else if (fps < 0) {
		setWindowTitle(tr("%1 - %2%3 - %4").arg(projectName).arg(title).arg(m_flashgbxSaveUploadBusy ? tr(" - Saving to Cartridge") : QString()).arg(projectVersion));
	} else {
		setWindowTitle(tr("%1 - %2 (%3 fps)%4 - %5").arg(projectName).arg(title).arg(fps).arg(m_flashgbxSaveUploadBusy ? tr(" - Saving to Cartridge") : QString()).arg(projectVersion));
	}
	updateFlashGBXRestrictedActions();
}

void Window::openStateWindow(LoadSave ls) {
	if (blockFlashGBXCartridgeRestrictedAction()) {
		return;
	}
	if (m_stateWindow) {
		return;
	}
	MultiplayerController* multiplayer = m_controller->multiplayerController();
	if (multiplayer && multiplayer->attached() > 1) {
		return;
	}
	bool wasPaused = m_controller->isPaused();
	m_stateWindow = new LoadSaveState(m_controller);
	connect(this, &Window::shutdown, m_stateWindow, &QWidget::close);
	connect(m_stateWindow, &LoadSaveState::closed, [this]() {
		attachWidget(m_display.get());
		m_stateWindow = nullptr;
		QMetaObject::invokeMethod(this, "setFocus", Qt::QueuedConnection);
	});
	if (!wasPaused) {
		m_controller->setPaused(true);
		connect(m_stateWindow, &LoadSaveState::closed, [this]() {
			if (m_controller) {
				m_controller->setPaused(false);
			}
		});
	}
	m_stateWindow->setAttribute(Qt::WA_DeleteOnClose);
	m_stateWindow->setMode(ls);

	m_stateWindow->setDimensions(m_controller->screenDimensions());
	m_config->updateOption("lockAspectRatio");
	m_config->updateOption("lockIntegerScaling");

	QImage still(m_controller->getPixels());
	if (still.format() != QImage::Format_RGB888) {
		still = still.convertToFormat(QImage::Format_RGB888);
	}
	if (still.height() > 512 || still.width() > 512) {
		still = still.scaled(384, 256, Qt::KeepAspectRatio, Qt::SmoothTransformation).convertToFormat(QImage::Format_RGB888);
	}
	QImage output(still.size(), QImage::Format_RGB888);
	size_t dims[] = {7, 7};
	struct ConvolutionKernel kern;
	ConvolutionKernelCreate(&kern, 2, dims);
	ConvolutionKernelFillRadial(&kern, true);
	Convolve2DClampChannels8(still.constBits(), output.bits(), still.width(), still.height(), still.bytesPerLine(), 3, &kern);
	ConvolutionKernelDestroy(&kern);

	QPixmap pixmap;
	pixmap.convertFromImage(output);
	m_stateWindow->setBackground(pixmap);

#ifndef Q_OS_MAC
	showMenu(true);
#endif
	attachWidget(m_stateWindow);
}

void Window::setupMenu(QMenuBar* menubar) {
	installEventFilter(m_shortcutController);

	menubar->clear();
	m_actions.addMenu(tr("&File"), "file");

	blockFlashGBXSaveUploadAction(m_actions.addAction(tr("Load &ROM..."), "loadROM", this, &Window::selectROM, "file", QKeySequence::Open));
	blockFlashGBXSaveUploadAction(m_actions.addAction(tr("Load from Cartridge..."), "loadFlashGBXCartridge", this, &Window::selectFlashGBXCartridge, "file"));

#ifdef USE_SQLITE3
	blockFlashGBXSaveUploadAction(m_actions.addAction(tr("Load ROM in archive..."), "loadROMInArchive", this, &Window::selectROMInArchive, "file"));
	m_actions.addAction(tr("Add folder to library..."), "addDirToLibrary", this, &Window::addDirToLibrary, "file");
#endif

	m_actions.addMenu(tr("Save games"), "saves", "file");
	auto loadAlternateSave = addGameAction(tr("Load alternate save game..."), "loadAlternateSave", [this]() {
		this->selectSave(false);
	}, "saves");
	restrictFlashGBXAction(loadAlternateSave);
	auto loadTemporarySave = addGameAction(tr("Load temporary save game..."), "loadTemporarySave", [this]() {
		this->selectSave(true);
	}, "saves");
	restrictFlashGBXAction(loadTemporarySave);

	m_actions.addSeparator("saves");

	m_actions.addAction(tr("Convert save game..."), "convertSave", openTView<SaveConverter>(), "saves");
	auto uploadFlashGBXSaveAction = addGameAction(tr("Upload Save to Cartridge Now"), "uploadFlashGBXSave", [this]() {
		this->uploadFlashGBXSave(true);
	}, "saves");
	m_nonMpActions.append(uploadFlashGBXSaveAction);

#ifdef M_CORE_GBA
	auto importShark = addGameAction(tr("Import GameShark Save..."), "importShark", this, &Window::importSharkport, "saves");
	m_platformActions.insert(mPLATFORM_GBA, importShark);
	restrictFlashGBXAction(importShark);

	auto exportShark = addGameAction(tr("Export GameShark Save..."), "exportShark", this, &Window::exportSharkport, "saves");
	m_platformActions.insert(mPLATFORM_GBA, exportShark);
#endif

	m_actions.addSeparator("saves");
	std::shared_ptr<Action> savePlayerAction;
	ConfigOption* savePlayer = m_config->addOption("savePlayerId");
	savePlayerAction = savePlayer->addValue(tr("Automatically determine"), 0, &m_actions, "saves");
	m_nonMpActions.append(savePlayerAction);
	restrictFlashGBXAction(savePlayerAction);

	for (int i = 1; i < 5; ++i) {
		savePlayerAction = savePlayer->addValue(tr("Use player %0 save game").arg(i), i, &m_actions, "saves");
		m_nonMpActions.append(savePlayerAction);
		restrictFlashGBXAction(savePlayerAction);
	}
	savePlayer->connect([this](const QVariant& value) {
		if (m_controller) {
			m_controller->changePlayer(value.toInt());
		}
	}, this);
	m_config->updateOption("savePlayerId");

	auto loadPatch = m_actions.addAction(tr("Load &patch..."), "loadPatch", this, &Window::selectPatch, "file");
	restrictFlashGBXAction(loadPatch);

#ifdef M_CORE_GBA
	blockFlashGBXSaveUploadAction(m_actions.addAction(tr("Boot BIOS"), "bootBIOS", this, &Window::bootBIOS, "file"));
#endif

#ifdef M_CORE_GBA
	auto scanCard = addGameAction(tr("Scan e-Reader dotcodes..."), "scanCard", this, &Window::scanCard, "file");
	m_platformActions.insert(mPLATFORM_GBA, scanCard);
#endif

	addGameAction(tr("ROM &info..."), "romInfo", openControllerTView<ROMInfo>(), "file");

	m_actions.addMenu(tr("Recent"), "mru", "file");
	m_actions.addSeparator("file");

	auto loadState = addGameAction(tr("&Load state"), "loadState", [this]() {
		this->openStateWindow(LoadSave::LOAD);
	}, "file", QKeySequence("F10"));
	m_nonMpActions.append(loadState);
	restrictFlashGBXAction(loadState);

	auto loadStateFile = addGameAction(tr("Load state file..."), "loadStateFile", [this]() {
		this->selectState(true);
	}, "file");
	m_nonMpActions.append(loadStateFile);
	restrictFlashGBXAction(loadStateFile);

	auto saveState = addGameAction(tr("&Save state"), "saveState", [this]() {
		this->openStateWindow(LoadSave::SAVE);
	}, "file", QKeySequence("Shift+F10"));
	m_nonMpActions.append(saveState);
	restrictFlashGBXAction(saveState);

	auto saveStateFile = addGameAction(tr("Save state file..."), "saveStateFile", [this]() {
		this->selectState(false);
	}, "file");
	m_nonMpActions.append(saveStateFile);
	restrictFlashGBXAction(saveStateFile);

	m_actions.addMenu(tr("Quick load"), "quickLoad", "file");
	m_actions.addMenu(tr("Quick save"), "quickSave", "file");

	auto quickLoad = addGameAction(tr("Load recent"), "quickLoad", [this] {
		loadFlashGBXSafeState();
	}, "quickLoad");
	m_nonMpActions.append(quickLoad);

	auto quickSave = addGameAction(tr("Save recent"), "quickSave", [this] {
		m_controller->saveState();
	}, "quickSave");
	m_nonMpActions.append(quickSave);

	m_actions.addSeparator("quickLoad");
	m_actions.addSeparator("quickSave");

	auto undoLoadState = addGameAction(tr("Undo load state"), "undoLoadState", &CoreController::loadBackupState, "quickLoad", QKeySequence("F11"));
	m_nonMpActions.append(undoLoadState);
	restrictFlashGBXAction(undoLoadState);

	auto undoSaveState = addGameAction(tr("Undo save state"), "undoSaveState", &CoreController::saveBackupState, "quickSave", QKeySequence("Shift+F11"));
	m_nonMpActions.append(undoSaveState);
	restrictFlashGBXAction(undoSaveState);

	m_actions.addSeparator("quickLoad");
	m_actions.addSeparator("quickSave");

	for (int i = 1; i < 10; ++i) {
		auto quickLoad = addGameAction(tr("State &%1").arg(i),  QString("quickLoad.%1").arg(i), [this, i]() {
			loadFlashGBXSafeState(i);
		}, "quickLoad", QString("F%1").arg(i));
		m_nonMpActions.append(quickLoad);

		auto quickSave = addGameAction(tr("State &%1").arg(i),  QString("quickSave.%1").arg(i), [this, i]() {
			m_controller->saveState(i);
		}, "quickSave", QString("Shift+F%1").arg(i));
		m_nonMpActions.append(quickSave);
	}

	m_actions.addSeparator("file");
	m_multiWindow = m_actions.addAction(tr("New multiplayer window"), "multiWindow", GBAApp::app(), &GBAApp::newWindow, "file");

#ifdef M_CORE_GBA
	auto dolphin = m_actions.addAction(tr("Connect to Dolphin..."), "connectDolphin", openNamedTView<DolphinConnector>(&m_dolphinView, true, this), "file");
	m_platformActions.insert(mPLATFORM_GBA, dolphin);
#endif

	m_actions.addSeparator("file");

	m_actions.addAction(tr("Report bug..."), "bugReport", openTView<ReportView>(), "file");

#ifndef Q_OS_MAC
	m_actions.addSeparator("file");
#endif

	m_actions.addAction(tr("About..."), "about", openTView<AboutScreen>(), "file")->setRole(Action::Role::ABOUT);
	auto quit = m_actions.addAction(tr("E&xit"), "quit", [this]() {
		if (!blockFlashGBXSaveUploadInProgress()) {
			QApplication::quit();
		}
	}, "file", QKeySequence::Quit);
	quit->setRole(Action::Role::QUIT);
	blockFlashGBXSaveUploadAction(quit);

	m_actions.addMenu(tr("&Emulation"), "emu");
	blockFlashGBXSaveUploadAction(addGameAction(tr("&Reset"), "reset", [this]() {
		if (!blockFlashGBXSaveUploadInProgress()) {
			m_controller->reset();
		}
	}, "emu", QKeySequence("Ctrl+R")));
	blockFlashGBXSaveUploadAction(addGameAction(tr("Sh&utdown"), "shutdown", [this]() {
		if (!blockFlashGBXSaveUploadInProgress()) {
			m_controller->stop();
		}
	}, "emu"));
	m_actions.addSeparator("emu");

	auto replaceROM = addGameAction(tr("Replace ROM..."), "replaceROM", this, &Window::replaceROM, "emu");
	restrictFlashGBXAction(replaceROM);
	auto yank = addGameAction(tr("Yank game pak"), "yank", &CoreController::yankPak, "emu");
	restrictFlashGBXAction(yank);
	m_actions.addSeparator("emu");

	auto pause = m_actions.addBooleanAction(tr("&Pause"), "pause", [this](bool paused) {
		if (m_controller) {
			m_controller->setPaused(paused);
		} else {
			m_pendingPause = paused;
		}
	}, "emu", QKeySequence("Ctrl+P"));
	connect(this, &Window::paused, pause.get(), &Action::setActive);

	addGameAction(tr("&Next frame"), "frameAdvance", &CoreController::frameAdvance, "emu", QKeySequence("Ctrl+N"));

	m_actions.addSeparator("emu");

	m_actions.addHeldAction(tr("Fast forward (held)"), "holdFastForward", [this](bool held) {
		if (m_controller) {
			m_controller->setFastForward(held);
		}
	}, "emu", QKeySequence(Qt::Key_Tab));

	addGameAction(tr("&Fast forward"), "fastForward", [this](bool value) {
		m_controller->forceFastForward(value);
	}, "emu", QKeySequence("Shift+Tab"));

	m_actions.addMenu(tr("Fast forward speed"), "fastForwardSpeed", "emu");
	ConfigOption* ffspeed = m_config->addOption("fastForwardRatio");
	ffspeed->connect([this](const QVariant&) {
		reloadConfig();
	}, this);
	ffspeed->addValue(tr("Unbounded"), -1.0f, &m_actions, "fastForwardSpeed");
	ffspeed->setValue(QVariant(-1.0f));
	m_actions.addSeparator("fastForwardSpeed");
	for (int i = 2; i < 11; ++i) {
		ffspeed->addValue(tr("%0x").arg(i), i, &m_actions, "fastForwardSpeed");
	}
	m_config->updateOption("fastForwardRatio");

	addGameAction(tr("Increase fast forward speed"), "fastForwardUp", [this] {
		float newRatio = m_config->getOption("fastForwardRatio", 1.0f).toFloat() + 1.0f;
		if (newRatio >= 3.0f) {
			m_config->setOption("fastForwardRatio", QVariant(newRatio));
		}
	}, "emu");

	addGameAction(tr("Decrease fast forward speed"), "fastForwardDown", [this] {
		float newRatio = m_config->getOption("fastForwardRatio").toFloat() - 1.0f;
		if (newRatio >= 2.0f) {
			m_config->setOption("fastForwardRatio", QVariant(newRatio));
		}
	}, "emu");

	auto rewindHeld = m_actions.addHeldAction(tr("Rewind (held)"), "holdRewind", [this](bool held) {
		// Prevent rewinding while the load/save state window is active
		if (held && this->m_stateWindow != nullptr) {
			return;
		}

		if (m_controller) {
			m_controller->setRewinding(held);
		}
	}, "emu", QKeySequence("`"));
	m_nonMpActions.append(rewindHeld);
	restrictFlashGBXAction(rewindHeld);

	auto rewind = addGameAction(tr("Re&wind"), "rewind", [this]() {
		m_controller->rewind();
	}, "emu", QKeySequence("~"));
	m_nonMpActions.append(rewind);
	restrictFlashGBXAction(rewind);

	auto frameRewind = addGameAction(tr("Step backwards"), "frameRewind", [this] () {
		m_controller->rewind(1);
	}, "emu", QKeySequence("Ctrl+B"));
	m_nonMpActions.append(frameRewind);
	restrictFlashGBXAction(frameRewind);

	m_actions.addSeparator("emu");

	m_actions.addMenu(tr("Solar sensor"), "solar", "emu");
	m_actions.addAction(tr("Increase solar level"), "increaseLuminanceLevel", &m_inputController, &InputController::increaseLuminanceLevel, "solar");
	m_actions.addAction(tr("Decrease solar level"), "decreaseLuminanceLevel", &m_inputController, &InputController::decreaseLuminanceLevel, "solar");
	m_actions.addAction(tr("Brightest solar level"), "maxLuminanceLevel", [this]() {
		m_inputController.setLuminanceLevel(10);
	}, "solar");
	m_actions.addAction(tr("Darkest solar level"), "minLuminanceLevel", [this]() {
		m_inputController.setLuminanceLevel(0);
	}, "solar");

	m_actions.addSeparator("solar");
	for (int i = 0; i <= 10; ++i) {
		m_actions.addAction(tr("Brightness %1").arg(QString::number(i)), QString("luminanceLevel.%1").arg(QString::number(i)), [this, i]() {
			m_inputController.setLuminanceLevel(i);
		}, "solar");
	}

#ifdef M_CORE_GB
	m_actions.addAction(tr("Load camera image..."), "loadCamImage", this, &Window::loadCamImage, "emu");

	auto gbPrint = addGameAction(tr("Game Boy Printer..."), "gbPrint", [this]() {
		PrinterView* view = new PrinterView(m_controller);
		openView(view);
		m_controller->attachPrinter();
	}, "emu");
	m_platformActions.insert(mPLATFORM_GB, gbPrint);
#endif

#ifdef M_CORE_GBA
	auto bcGate = addGameAction(tr("BattleChip Gate..."), "bcGate", openControllerTView<BattleChipView>(this), "emu");
	m_platformActions.insert(mPLATFORM_GBA, bcGate);
#endif

	m_actions.addMenu(tr("Audio/&Video"), "av");
	m_actions.addMenu(tr("Frame size"), "frame", "av");
	for (int i = 1; i <= 8; ++i) {
		auto setSize = m_actions.addAction(tr("%1×").arg(QString::number(i)), QString("frame.%1x").arg(QString::number(i)), [this, i]() {
			auto setSize = m_frameSizes[i];
			bool lockFrameSize = m_config->getOption("lockFrameSize").toInt();
			if (!lockFrameSize) {
				showNormal();
			}
#if defined(M_CORE_GBA)
			QSize minimumSize = QSize(GBA_VIDEO_HORIZONTAL_PIXELS, GBA_VIDEO_VERTICAL_PIXELS);
#elif defined(M_CORE_GB)
			QSize minimumSize = QSize(GB_VIDEO_HORIZONTAL_PIXELS, GB_VIDEO_VERTICAL_PIXELS);
#endif
			QSize size;
			if (m_display) {
				size = m_display->contentSize();
			}
			if (size.isNull()) {
				size = minimumSize;
			}
			size *= i;
			m_savedScale = i;
			m_config->setOption("scaleMultiplier", i); // TODO: Port to other
			m_savedSize = size;
			resizeFrame(size);
			if (lockFrameSize) {
				m_display->setMaximumSize(size);
			}
			setSize->setActive(true);
		}, "frame");
		setSize->setExclusive(true);
		if (m_savedScale == i) {
			setSize->setActive(true);
		}
		m_frameSizes[i] = setSize;
	}
	QKeySequence fullscreenKeys;
#ifdef Q_OS_WIN
	fullscreenKeys = QKeySequence("Alt+Return");
#else
	fullscreenKeys = QKeySequence("Ctrl+F");
#endif
	m_actions.addSeparator("frame");
	m_actions.addAction(tr("Toggle fullscreen"), "fullscreen", this, &Window::toggleFullScreen, "frame", fullscreenKeys);

	ConfigOption* lockFrameSize = m_config->addOption("lockFrameSize");
	lockFrameSize->addBoolean(tr("&Lock frame size"), &m_actions, "frame");
	lockFrameSize->connect([this](const QVariant& value) {
		if (m_display) {
			if (value.toBool()) {
				m_display->setMaximumSize(m_display->size());
			} else {
				m_display->setMaximumSize({});
			}
		}
	}, this);
	m_config->updateOption("lockFrameSize");

	ConfigOption* lockAspectRatio = m_config->addOption("lockAspectRatio");
	lockAspectRatio->addBoolean(tr("Lock aspect ratio"), &m_actions, "av");
	lockAspectRatio->connect([this](const QVariant& value) {
		if (m_display) {
			m_display->lockAspectRatio(value.toBool());
		}
		if (m_stateWindow) {
			m_stateWindow->setLockAspectRatio(value.toBool());
		}
	}, this);
	m_config->updateOption("lockAspectRatio");

	ConfigOption* lockIntegerScaling = m_config->addOption("lockIntegerScaling");
	lockIntegerScaling->addBoolean(tr("Force integer scaling"), &m_actions, "av");
	lockIntegerScaling->connect([this](const QVariant& value) {
		if (m_display) {
			m_display->lockIntegerScaling(value.toBool());
		}
		if (m_stateWindow) {
			m_stateWindow->setLockIntegerScaling(value.toBool());
		}
	}, this);
	m_config->updateOption("lockIntegerScaling");

	ConfigOption* interframeBlending = m_config->addOption("interframeBlending");
	interframeBlending->addBoolean(tr("Interframe blending"), &m_actions, "av");
	interframeBlending->connect([this](const QVariant& value) {
		if (m_display) {
			m_display->interframeBlending(value.toBool());
		}
	}, this);
	m_config->updateOption("interframeBlending");

	ConfigOption* resampleVideo = m_config->addOption("resampleVideo");
	resampleVideo->addBoolean(tr("Bilinear filtering"), &m_actions, "av");
	resampleVideo->connect([this](const QVariant& value) {
		if (m_display) {
			m_display->filter(value.toBool());
		}
	}, this);
	m_config->updateOption("resampleVideo");

	m_actions.addMenu(tr("Frame&skip"),"skip", "av");
	ConfigOption* skip = m_config->addOption("frameskip");
	skip->connect([this](const QVariant&) {
		reloadConfig();
	}, this);
	for (int i = 0; i <= 10; ++i) {
		skip->addValue(QString::number(i), i, &m_actions, "skip");
	}
	m_config->updateOption("frameskip");

	m_actions.addSeparator("av");

	ConfigOption* mute = m_config->addOption("mute");
	auto muteAction = mute->addBoolean(tr("Mute"), &m_actions, "av");
	muteAction->setActive(m_config->getOption("mute").toInt());
	mute->connect([this](const QVariant& value) {
		m_config->setOption("fastForwardMute", static_cast<bool>(value.toInt()));
		reloadConfig();
	}, this);

	m_actions.addMenu(tr("FPS target"),"target", "av");
	ConfigOption* fpsTargetOption = m_config->addOption("fpsTarget");
	QMap<double, std::shared_ptr<Action>> fpsTargets;
	for (int fps : {15, 30, 45, 60, 90, 120, 240}) {
		fpsTargets[fps] = fpsTargetOption->addValue(QString::number(fps), fps, &m_actions, "target");
	}
	m_actions.addSeparator("target");
	double nativeGB = double(GBA_ARM7TDMI_FREQUENCY) / double(VIDEO_TOTAL_LENGTH);
	fpsTargets[nativeGB] = fpsTargetOption->addValue(tr("Native (59.7275)"), nativeGB, &m_actions, "target");

	fpsTargetOption->connect([this, fpsTargets = std::move(fpsTargets)](const QVariant& value) {
		reloadConfig();
		for (auto iter = fpsTargets.begin(); iter != fpsTargets.end(); ++iter) {
			bool enableSignals = iter.value()->blockSignals(true);
			iter.value()->setActive(abs(iter.key() - value.toDouble()) < 0.001);
			iter.value()->blockSignals(enableSignals);
		}
	}, this);
	m_config->updateOption("fpsTarget");

	m_actions.addSeparator("av");

#ifdef USE_PNG
	addGameAction(tr("Take &screenshot"), "screenshot", [this]() {
		m_controller->screenshot();
	}, "av", tr("F12"));
#endif

#ifdef USE_FFMPEG
	addGameAction(tr("Record A/V..."), "recordOutput", openNamedControllerTView<VideoView>(&m_videoView, true), "av");
	addGameAction(tr("Record GIF/WebP/APNG..."), "recordGIF", openNamedControllerTView<GIFView>(&m_gifView, true), "av");
#endif

	m_actions.addSeparator("av");
	m_actions.addMenu(tr("Video layers"), "videoLayers", "av");
	m_actions.addMenu(tr("Audio channels"), "audioChannels", "av");

	addGameAction(tr("Adjust layer placement..."), "placementControl", openControllerTView<PlacementControl>(), "av");

#ifndef Q_OS_MAC
	m_actions.addSeparator("av");
	m_actions.addBooleanAction(tr("Hide &menu"), "hideMenu", [this](bool hidden) {
		showMenu(!hidden);
	}, "av", QKeySequence("Ctrl+M"));
#endif

	m_actions.addMenu(tr("&Tools"), "tools");
	m_actions.addAction(tr("View &logs..."), "viewLogs", static_cast<QWidget*>(m_logView), &QWidget::show, "tools");

	m_actions.addAction(tr("Game &overrides..."), "overrideWindow", [this]() {
		if (!m_overrideView) {
			m_overrideView = new OverrideView(m_config);
			if (m_controller) {
				m_overrideView->setController(m_controller);
			}
			connect(this, &Window::shutdown, m_overrideView.data(), &QWidget::close);
		}
		m_overrideView->show();
		m_overrideView->activateWindow();
		m_overrideView->raise();
	}, "tools");

	m_actions.addAction(tr("Game Pak sensors..."), "sensorWindow", [this]() {
		if (!m_sensorView) {
			m_sensorView = new SensorView(&m_inputController);
			if (m_controller) {
				m_sensorView->setController(m_controller);
			}
			connect(this, &Window::shutdown, m_sensorView.data(), &QWidget::close);
		}
		m_sensorView->show();
		m_sensorView->activateWindow();
		m_sensorView->raise();
	}, "tools");

	addGameAction(tr("&Cheats..."), "cheatsWindow", openControllerTView<CheatsView>(), "tools");
#ifdef ENABLE_SCRIPTING
	m_actions.addAction(tr("Scripting..."), "scripting", this, &Window::scriptingOpen, "tools");
#endif

	m_actions.addAction(tr("Create forwarder..."), "createForwarder", openTView<ForwarderView>(), "tools");

	m_actions.addSeparator("tools");
	m_actions.addAction(tr("Settings..."), "settings", this, &Window::openSettingsWindow, "tools")->setRole(Action::Role::SETTINGS);
	m_actions.addAction(tr("Make portable"), "makePortable", this, &Window::tryMakePortable, "tools");

	m_actions.addSeparator("tools");
#ifdef ENABLE_DEBUGGERS
	m_actions.addAction(tr("Open debugger console..."), "debuggerWindow", this, &Window::consoleOpen, "tools");
#ifdef ENABLE_GDB_STUB
	auto gdbWindow = addGameAction(tr("Start &GDB server..."), "gdbWindow", this, &Window::gdbOpen, "tools");
	m_platformActions.insert(mPLATFORM_GBA, gdbWindow);
#endif
#endif
#if defined(ENABLE_DEBUGGERS) || defined(ENABLE_SCRIPTING)
	m_actions.addSeparator("tools");
#endif

	m_actions.addMenu(tr("Game state views"), "stateViews", "tools");
	addGameAction(tr("View &palette..."), "paletteWindow", openControllerTView<PaletteView>(), "stateViews");
	addGameAction(tr("View &sprites..."), "spriteWindow", openControllerTView<ObjView>(), "stateViews");
	addGameAction(tr("View &tiles..."), "tileWindow", openControllerTView<TileView>(), "stateViews");
	addGameAction(tr("View &map..."), "mapWindow", openControllerTView<MapView>(), "stateViews");
	addGameAction(tr("&Frame inspector..."), "frameWindow", openNamedControllerTView<FrameView>(&m_frameView, false), "stateViews");
	addGameAction(tr("View memory..."), "memoryView", openControllerTView<MemoryView>(), "stateViews");
	addGameAction(tr("Search memory..."), "memorySearch", openControllerTView<MemorySearch>(), "stateViews");
	addGameAction(tr("View &I/O registers..."), "ioViewer", openControllerTView<IOViewer>(), "stateViews");

#ifdef ENABLE_DEBUGGERS
	addGameAction(tr("Log memory &accesses..."), "memoryAccessView", [this]() {
		std::weak_ptr<MemoryAccessLogController> controller = m_controller->memoryAccessLogController();
		MemoryAccessLogView* view = new MemoryAccessLogView(controller);
		connect(m_controller.get(), &CoreController::stopping, view, &QWidget::close);
		openView(view);
	}, "tools");
#endif

#if defined(USE_FFMPEG) && defined(M_CORE_GBA)
	m_actions.addSeparator("tools");
	m_actions.addAction(tr("Convert e-Reader card image to raw..."), "parseCard", this, &Window::parseCard, "tools");
#endif

	m_actions.addSeparator("tools");
	addGameAction(tr("Record debug video log..."), "recordVL", this, &Window::startVideoLog, "tools");
	addGameAction(tr("Stop debug video log"), "stopVL", [this]() {
		m_controller->endVideoLog();
	}, "tools");

	m_actions.addHiddenAction(tr("Exit fullscreen"), "exitFullScreen", this, &Window::exitFullScreen, "frame", QKeySequence("Esc"));

	m_actions.addHeldAction(tr("GameShark Button (held)"), "holdGSButton", [this](bool held) {
		if (m_controller) {
			mCheatPressButton(m_controller->cheatDevice(), held);
		}
	}, "tools");

	m_actions.addHiddenMenu(tr("Autofire"), "autofire");
	m_actions.addHeldAction(tr("Autofire A"), "autofireA", [this](bool held) {
		if (m_controller) {
			m_controller->setAutofire(GBA_KEY_A, held);
		}
	}, "autofire");
	m_actions.addHeldAction(tr("Autofire B"), "autofireB", [this](bool held) {
		if (m_controller) {
			m_controller->setAutofire(GBA_KEY_B, held);
		}
	}, "autofire");
	m_actions.addHeldAction(tr("Autofire L"), "autofireL", [this](bool held) {
		if (m_controller) {
			m_controller->setAutofire(GBA_KEY_L, held);
		}
	}, "autofire");
	m_actions.addHeldAction(tr("Autofire R"), "autofireR", [this](bool held) {
		if (m_controller) {
			m_controller->setAutofire(GBA_KEY_R, held);
		}
	}, "autofire");
	m_actions.addHeldAction(tr("Autofire Start"), "autofireStart", [this](bool held) {
		if (m_controller) {
			m_controller->setAutofire(GBA_KEY_START, held);
		}
	}, "autofire");
	m_actions.addHeldAction(tr("Autofire Select"), "autofireSelect", [this](bool held) {
		if (m_controller) {
			m_controller->setAutofire(GBA_KEY_SELECT, held);
		}
	}, "autofire");
	m_actions.addHeldAction(tr("Autofire Up"), "autofireUp", [this](bool held) {
		if (m_controller) {
			m_controller->setAutofire(GBA_KEY_UP, held);
		}
	}, "autofire");
	m_actions.addHeldAction(tr("Autofire Right"), "autofireRight", [this](bool held) {
		if (m_controller) {
			m_controller->setAutofire(GBA_KEY_RIGHT, held);
		}
	}, "autofire");
	m_actions.addHeldAction(tr("Autofire Down"), "autofireDown", [this](bool held) {
		if (m_controller) {
			m_controller->setAutofire(GBA_KEY_DOWN, held);
		}
	}, "autofire");
	m_actions.addHeldAction(tr("Autofire Left"), "autofireLeft", [this](bool held) {
		if (m_controller) {
			m_controller->setAutofire(GBA_KEY_LEFT, held);
		}
	}, "autofire");

	for (auto& action : m_gameActions) {
		action->setEnabled(false);
	}

	m_shortcutController->rebuildItems();
	m_actions.rebuildMenu(menuBar(), this, *m_shortcutController);
}

void Window::setupOptions() {
	ConfigOption* videoSync = m_config->addOption("videoSync");
	videoSync->connect([this](const QVariant&) {
		reloadConfig();
	}, this);

	ConfigOption* audioSync = m_config->addOption("audioSync");
	audioSync->connect([this](const QVariant&) {
		reloadConfig();
	}, this);

	ConfigOption* skipBios = m_config->addOption("skipBios");
	skipBios->connect([this](const QVariant&) {
		reloadConfig();
	}, this);

	ConfigOption* useBios = m_config->addOption("useBios");
	useBios->connect([this](const QVariant&) {
		reloadConfig();
	}, this);

	ConfigOption* buffers = m_config->addOption("audioBuffers");
	buffers->connect([this](const QVariant&) {
		reloadConfig();
	}, this);

	ConfigOption* sampleRate = m_config->addOption("sampleRate");
	sampleRate->connect([this](const QVariant&) {
		reloadConfig();
	}, this);

	ConfigOption* volume = m_config->addOption("volume");
	volume->connect([this](const QVariant&) {
		reloadConfig();
	}, this);

	ConfigOption* volumeFf = m_config->addOption("fastForwardVolume");
	volumeFf->connect([this](const QVariant&) {
		reloadConfig();
	}, this);

	ConfigOption* muteFf = m_config->addOption("fastForwardMute");
	muteFf->connect([this](const QVariant&) {
		reloadConfig();
	}, this);

	ConfigOption* rewindEnable = m_config->addOption("rewindEnable");
	rewindEnable->connect([this](const QVariant&) {
		reloadConfig();
	}, this);

	ConfigOption* rewindBufferCapacity = m_config->addOption("rewindBufferCapacity");
	rewindBufferCapacity->connect([this](const QVariant&) {
		reloadConfig();
	}, this);

	ConfigOption* rewindBufferInterval = m_config->addOption("rewindBufferInterval");
	rewindBufferInterval->connect([this](const QVariant&) {
		reloadConfig();
	}, this);

	ConfigOption* allowOpposingDirections = m_config->addOption("allowOpposingDirections");
	allowOpposingDirections->connect([this](const QVariant&) {
		reloadConfig();
	}, this);

	ConfigOption* saveStateExtdata = m_config->addOption("saveStateExtdata");
	saveStateExtdata->connect([this](const QVariant&) {
		reloadConfig();
	}, this);

	ConfigOption* loadStateExtdata = m_config->addOption("loadStateExtdata");
	loadStateExtdata->connect([this](const QVariant&) {
		reloadConfig();
	}, this);

	ConfigOption* preload = m_config->addOption("preload");
	preload->connect([this](const QVariant& value) {
		m_manager->setPreload(value.toBool());
	}, this);
	m_config->updateOption("preload");

	ConfigOption* showFps = m_config->addOption("showFps");
	showFps->connect([this](const QVariant& value) {
		if (!value.toInt()) {
			m_fpsTimer.stop();
			updateTitle();
		} else if (m_controller) {
			m_fpsTimer.start();
			m_frameTimer.start();
		}
	}, this);

	ConfigOption* showOSD = m_config->addOption("showOSD");
	showOSD->connect([this](const QVariant& value) {
		if (m_display && !value.isNull()) {
			m_display->showOSDMessages(value.toBool());
		}
	}, this);

	ConfigOption* showFrameCounter = m_config->addOption("showFrameCounter");
	showFrameCounter->connect([this](const QVariant& value) {
		if (m_display) {
			m_display->showFrameCounter(value.toBool());
		}
	}, this);

	ConfigOption* showResetInfo = m_config->addOption("showResetInfo");
	showResetInfo->connect([this](const QVariant& value) {
		if (m_controller) {
			m_controller->showResetInfo(value.toBool());
		}
	}, this);

	ConfigOption* videoScale = m_config->addOption("videoScale");
	videoScale->connect([this](const QVariant& value) {
		if (m_display) {
			m_display->setVideoScale(value.toInt());
#ifdef ENABLE_SCRIPTING
			if (m_controller && m_scripting) {
				m_scripting->updateVideoScale();
			}
#endif
		}
	}, this);

	ConfigOption* dynamicTitle = m_config->addOption("dynamicTitle");
	dynamicTitle->connect([this](const QVariant&) {
		updateTitle();
	}, this);

	ConfigOption* backgroundImage = m_config->addOption("backgroundImage");
	backgroundImage->connect([this](const QVariant& value) {
		if (m_display) {
			QString backgroundImage = value.toString();
			if (backgroundImage.isEmpty()) {
				m_display->setBackgroundImage(QImage{});
			} else {
				m_display->setBackgroundImage(QImage{backgroundImage});
			}
		}
	}, this);
	m_config->updateOption("backgroundImage");
}

void Window::attachWidget(QWidget* widget) {
	// Fix https://mgba.io/i/2885 -- seems like a Qt bug
	if (m_display && widget != m_display.get()) {
		m_display->hide();
	}
	takeCentralWidget();
	setCentralWidget(widget);
	if (m_display && widget == m_display.get()) {
		m_display->show();
	}
}

void Window::detachWidget() {
	m_config->updateOption("showLibrary");
}

void Window::appendMRU(const QString& fname) {
	int index = m_mruFiles.indexOf(fname);
	if (index >= 0) {
		m_mruFiles.removeAt(index);
	}
	m_mruFiles.prepend(fname);
	while (m_mruFiles.size() > ConfigController::MRU_LIST_SIZE) {
		m_mruFiles.removeLast();
	}
	updateMRU();
}

void Window::clearMRU() {
	m_mruFiles.clear();
	updateMRU();
}

void Window::updateMRU() {
	m_actions.clearMenu("mru");
	int i = 0;
	for (const QString& file : m_mruFiles) {
		QString displayName(QDir::toNativeSeparators(file).replace("&", "&&"));
		m_actions.addAction(displayName, QString("mru.%1").arg(QString::number(i)), [this, file]() {
			setController(m_manager->loadGame(file));
		}, "mru", QString("Ctrl+%1").arg(i));
		++i;
	}
	m_config->setMRU(m_mruFiles);
	m_config->write();
	m_actions.addSeparator("mru");
	m_actions.addAction(tr("Clear"), "resetMru", this, &Window::clearMRU, "mru");

	m_actions.rebuildMenu(menuBar(), this, *m_shortcutController);
}

void Window::ensureScripting() {
#ifdef ENABLE_SCRIPTING
	if (m_scripting) {
		return;
	}
	m_scripting = std::make_unique<ScriptingController>(m_config);
	m_scripting->setInputController(&m_inputController);
	m_shortcutController->setScriptingController(m_scripting.get());
	if (m_controller) {
		m_scripting->setController(m_controller);
		m_display->installEventFilter(m_scripting.get());
	}

	if (m_display) {
		m_scripting->setVideoBackend(m_display->videoBackend());
	}

	connect(m_scripting.get(), &ScriptingController::autorunScriptsOpened, this, &Window::openView);
#endif
}

std::shared_ptr<Action> Window::addGameAction(const QString& visibleName, const QString& name, Action::Function function, const QString& menu, const QKeySequence& shortcut) {
	auto action = m_actions.addAction(visibleName, name, [this, function = std::move(function)]() {
		if (m_controller) {
			function();
		}
	}, menu, shortcut);
	m_gameActions.append(action);
	return action;
}

template<typename T, typename V>
std::shared_ptr<Action> Window::addGameAction(const QString& visibleName, const QString& name, T* obj, V (T::*method)(), const QString& menu, const QKeySequence& shortcut) {
	return addGameAction(visibleName, name, [obj, method]() {
		(obj->*method)();
	}, menu, shortcut);
}

template<typename V>
std::shared_ptr<Action> Window::addGameAction(const QString& visibleName, const QString& name, V (CoreController::*method)(), const QString& menu, const QKeySequence& shortcut) {
	return addGameAction(visibleName, name, [this, method]() {
		(m_controller.get()->*method)();
	}, menu, shortcut);
}

std::shared_ptr<Action> Window::addGameAction(const QString& visibleName, const QString& name, Action::BooleanFunction function, const QString& menu, const QKeySequence& shortcut) {
	auto action = m_actions.addBooleanAction(visibleName, name, [this, function = std::move(function)](bool value) {
		if (m_controller) {
			function(value);
		}
	}, menu, shortcut);
	m_gameActions.append(action);
	return action;
}

void Window::focusCheck() {
	if (!m_controller) {
		return;
	}
	if (m_config->getOption("pauseOnFocusLost").toInt()) {
		if (QGuiApplication::focusWindow() && m_autoresume) {
			m_controller->setPaused(false);
			m_autoresume = false;
		} else if (!QGuiApplication::focusWindow() && !m_controller->isPaused()) {
			m_autoresume = true;
			m_controller->setPaused(true);
		}
	}
	if (m_config->getOption("muteOnFocusLost").toInt()) {
		if (QGuiApplication::focusWindow()) {
			m_inactiveMute = false;
		} else {
			m_inactiveMute = true;
		}
		updateMute();
	}
}

void Window::updateFrame() {
	if (!m_controller) {
		return;
	}
	QPixmap pixmap;
	pixmap.convertFromImage(m_controller->getPixels());
	m_screenWidget->setPixmap(pixmap);
}

void Window::setController(CoreController* controller) {
	if (!controller) {
		return;
	}
	if (m_pendingClose) {
		return;
	}

	if (m_controller) {
		m_controller->stop();
		QTimer::singleShot(0, this, [this, controller]() {
			setController(controller);
		});
		return;
	}

	if (m_flashgbxUseSoftwareDisplay) {
		m_flashgbxUseSoftwareDisplay = false;
		Display::setDriver(Display::Driver::QT);
		reloadDisplayDriver();
	}

	QString baseDirectory = controller->baseDirectory();
	QString path = controller->path();
	if (!path.isEmpty()) {
		QString fname;
		if (baseDirectory.isEmpty()) {
			fname = path;
		} else {
			fname = QFileInfo(QDir(baseDirectory), path).filePath();
		}
		if (!fname.isEmpty()) {
			setWindowFilePath(fname);
			appendMRU(fname);
		}
	}

	if (!m_display) {
		reloadDisplayDriver();
	}

	m_controller = std::shared_ptr<CoreController>(controller);
	m_controller->setInputController(&m_inputController);
	m_controller->setLogger(&m_log);

	connect(this, &Window::shutdown, [this]() {
		if (!m_controller) {
			return;
		}
		m_controller->stop();
		disconnect(m_controller.get(), &CoreController::started, this, &Window::gameStarted);
	});

	connect(m_controller.get(), &CoreController::started, this, &Window::gameStarted);
	connect(m_controller.get(), &CoreController::started, GBAApp::app(), &GBAApp::suspendScreensaver);
	connect(m_controller.get(), &CoreController::stopping, this, &Window::gameStopped);
	connect(m_controller.get(), &CoreController::stopping, GBAApp::app(), &GBAApp::resumeScreensaver);
	connect(m_controller.get(), &CoreController::paused, this, &Window::updateFrame);

#ifndef Q_OS_MAC
	connect(m_controller.get(), &CoreController::paused, [this]() {
		showMenu(true);
	});
	connect(m_controller.get(), &CoreController::unpaused, [this]() {
		if(isFullScreen()) {
			showMenu(false);
		}
	});
#endif

	connect(m_controller.get(), &CoreController::paused, GBAApp::app(), &GBAApp::resumeScreensaver);
	connect(m_controller.get(), &CoreController::paused, [this]() {
		emit paused(true);
	});
	connect(m_controller.get(), &CoreController::unpaused, [this]() {
		emit paused(false);
	});
	connect(m_controller.get(), &CoreController::unpaused, GBAApp::app(), &GBAApp::suspendScreensaver);
	connect(m_controller.get(), &CoreController::frameAvailable, this, &Window::recordFrame);
	connect(m_controller.get(), &CoreController::savedataUpdated, this, [this]() {
		if (!isFlashGBXCartridgeActive()) {
			return;
		}
		configureFlashGBXSaveWatcher();
		scheduleFlashGBXSaveUpload();
	});
	connect(m_controller.get(), &CoreController::crashed, this, &Window::gameCrashed);
	connect(m_controller.get(), &CoreController::failed, this, &Window::gameFailed);
	connect(m_controller.get(), &CoreController::unimplementedBiosCall, this, &Window::unimplementedBiosCall);

#ifdef M_CORE_GBA
	if (m_controller->platform() == mPLATFORM_GBA) {
		QVariant mb = m_config->takeArgvOption(QString("mb"));
#if (QT_VERSION < QT_VERSION_CHECK(6, 0, 0))
		if (mb.canConvert(QMetaType::QString)) {
#else
		if (QMetaType::canConvert(mb.metaType(), QMetaType(QMetaType::QString))) {
#endif
			m_controller->replaceGame(mb.toString());
		}
	}
#endif

#ifdef ENABLE_GDB_STUB
	if (m_gdbController) {
		m_gdbController->setController(m_controller);
	}
#endif

#ifdef ENABLE_DEBUGGERS
	if (m_console) {
		m_console->setController(m_controller);
	}
#endif

#ifdef USE_FFMPEG
	if (m_gifView) {
		m_gifView->setController(m_controller);
	}

	if (m_videoView) {
		m_videoView->setController(m_controller);
	}
#endif

	if (m_sensorView) {
		m_sensorView->setController(m_controller);
	}

	if (m_overrideView) {
		m_overrideView->setController(m_controller);
	}

	if (!m_pendingPatch.isEmpty()) {
		m_controller->loadPatch(m_pendingPatch);
		m_pendingPatch = QString();
	}

#ifdef ENABLE_SCRIPTING
	if (m_scripting) {
		m_scripting->setController(m_controller);

		m_scripting->setVideoBackend(m_display->videoBackend());
	}
#endif

	attachDisplay();
	m_controller->loadConfig(m_config);
	m_config->updateOption("showOSD");
	m_config->updateOption("showFrameCounter");
	m_config->updateOption("showResetInfo");
	m_controller->start();

	if (!m_pendingState.isEmpty()) {
		m_controller->loadState(m_pendingState);
		m_pendingState = QString();
	}

	if (m_pendingPause) {
		m_controller->setPaused(true);
		m_pendingPause = false;
	}

#ifdef ENABLE_SCRIPTING
	if (!m_scripting) {
		QStringList scripts = m_config->getArgvOption("script").toStringList();
		if (!scripts.isEmpty()) {
			scriptingOpen();
			for (const auto& scriptPath : scripts) {
				m_scripting->loadFile(scriptPath);
			}
		}
	}
#endif
}

void Window::attachDisplay() {
	m_display->attach(m_controller);
	connect(m_display.get(), &QGBA::Display::drawingStarted, this, &Window::changeRenderer);
	if (m_config->getOption("lockFrameSize").toInt()) {
		m_display->setMaximumSize(m_savedSize);
	} else {
		m_display->setMaximumSize({});
	}
	m_display->startDrawing(m_controller);

#ifdef ENABLE_SCRIPTING
	if (m_scripting) {
		m_display->installEventFilter(m_scripting.get());
	}
#endif
}

void Window::updateMute() {
	if (!m_controller) {
		return;
	}

	bool mute = m_inactiveMute || m_minimizedMute;

	if (!mute) {
		QString multiplayerAudio = m_config->getQtOption("multiplayerAudio").toString();
		if (multiplayerAudio == QLatin1String("p1")) {
			MultiplayerController* multiplayer = m_controller->multiplayerController();
			mute = multiplayer && multiplayer->attached() > 1 && m_playerId;
		} else if (multiplayerAudio == QLatin1String("active")) {
			mute = !m_multiActive;
		}
	}

	m_controller->overrideMute(mute);
}

void Window::setLogo() {
	m_screenWidget->setPixmap(m_logo);
	m_screenWidget->setDimensions(m_logo.width(), m_logo.height());
	centralWidget()->unsetCursor();
}

void Window::delayedCleanup() {
	// Destroy the controller after everything else has cleaned up, except for the display
	m_cleanupController.reset();
	// The display needs to be cleaned up last so the core can clean up the OpenGL resources
	m_cleanupDisplay.reset();
}

WindowBackground::WindowBackground(QWidget* parent)
	: QWidget(parent)
{
}

void WindowBackground::setPixmap(const QPixmap& pmap) {
	m_pixmap = pmap;
	update();
}

void WindowBackground::setSizeHint(const QSize& hint) {
	m_sizeHint = hint;
}

QSize WindowBackground::sizeHint() const {
	return m_sizeHint;
}

void WindowBackground::setDimensions(int width, int height) {
	m_aspectWidth = width;
	m_aspectHeight = height;
}

void WindowBackground::paintEvent(QPaintEvent* event) {
	QWidget::paintEvent(event);
	const QPixmap& logo = pixmap();
	QPainter painter(this);
	painter.setRenderHint(QPainter::SmoothPixmapTransform, true);
	painter.fillRect(QRect(QPoint(), size()), Qt::black);
	QRect full(clampSize(QSize(m_aspectWidth, m_aspectHeight), size(), true, false));
	painter.drawPixmap(full, logo);
}
