/**
 *  Fonts.cpp
 *  ONScripter-RU
 *
 *  Low level font control code based on freetype.
 *
 *  Consult LICENSE file for licensing terms and copyright holders.
 */

#include "Engine/Components/Fonts.hpp"
#include "Engine/Readers/Base.hpp"
#include "Engine/Core/ONScripter.hpp"
#include "Support/FileIO.hpp"

#include <sstream>

FontsController fonts;

bool FontsController::loadFont(Font &f, size_t i, bool user) {
	char *dir = user ? userfontdir : fontdir;
	if (dir[0] == '\0') {
		char *tmp = (*reader)->completePath(user ? "fonts/usr" : "fonts", FileType::Directory);
		if (!tmp) {
			// Font directory must exist, we no longer support root as a font directory.
			return false;
		}
		copystr(dir, tmp, sizeof(user ? userfontdir : fontdir));
		FileIO::terminatePath(dir, sizeof(user ? userfontdir : fontdir));
		freearr(&tmp);
	}

	const char *extensions[]{".ttf", ".otf"};
	bool found{false};
	size_t size{0};

	for (auto &ext : extensions) {
		char tmp[32]{};

		if (i == 0)
			std::snprintf(tmp, sizeof(tmp), "default%s", ext);
		else
			std::snprintf(tmp, sizeof(tmp), "font%zu%s", i, ext);

		if (FileIO::accessFile(tmp, dir, FileType::File, &size)) {
			size_t len = std::strlen(dir) + std::strlen(tmp) + 1;
			f.path = std::make_unique<char[]>(len);
			std::snprintf(f.path.get(), len, "%s%s", dir, tmp);
			found  = true;
		}
	}

	if (!found)
		return false;

	FILE *fp = FileIO::openFile(f.path.get(), "rb");

	if (!fp)
		return false;

	auto stream                = new FT_StreamRec{};
	stream->descriptor.pointer = fp;
	stream->size               = size;
	stream->read               = [](FT_Stream stream, unsigned long offset, unsigned char *buffer, unsigned long count) -> unsigned long {
		auto fp = static_cast<FILE *>(stream->descriptor.pointer);
		FileIO::seekFile(fp, offset, SEEK_SET);
		return std::fread(buffer, sizeof(uint8_t), count, fp);
	};
	stream->close = [](FT_Stream stream) -> void {
		std::fclose(static_cast<FILE *>(stream->descriptor.pointer));
		delete stream;
	};

	FT_Open_Args args{};
	args.flags  = FT_OPEN_STREAM;
	args.stream = stream;

	if (FT_Open_Face(freetype, &args, 0, &f.normal_face))
		return false;

	//Set normal_face as current face
	f.face = f.normal_face;

	//Time to load other typefaces
	FT_Long num = f.normal_face->num_faces;
	//sendToLog(LogLevel::Info, "Font %d with path %s has %d typefaces\n", i, fonts[i].path, num);

	if (num > 1) {
		//Start from 1, 0 is default
		for (FT_Long t = 1; t < num; t++) {
			FT_Face tmp_font_face;
			if (FT_New_Face(freetype, f.path.get(), t, &tmp_font_face)) {
				//sendToLog(LogLevel::Error, "Error at loading typface %d\n", t);
				FT_Done_Face(tmp_font_face);
				continue;
			}

			if (tmp_font_face->style_flags == (FT_STYLE_FLAG_ITALIC | FT_STYLE_FLAG_BOLD) && !f.hasInternalBoldItalicFace) {
				f.bold_italic_face          = tmp_font_face;
				f.hasInternalBoldItalicFace = true;
				//sendToLog(LogLevel::Info, "bold_italic_face loaded\n");
				continue;
			} else if (tmp_font_face->style_flags == FT_STYLE_FLAG_BOLD && !f.hasInternalBoldFace) {
				f.bold_face           = tmp_font_face;
				f.hasInternalBoldFace = true;
				//sendToLog(LogLevel::Info, "bold_face loaded\n");
				continue;
			} else if (tmp_font_face->style_flags == FT_STYLE_FLAG_ITALIC && !f.hasInternalItalicFace) {
				f.italic_face           = tmp_font_face;
				f.hasInternalItalicFace = true;
				//sendToLog(LogLevel::Info, "italic_face loaded\n");
				continue;
			}
			//Something unrelated
			FT_Done_Face(tmp_font_face);
		}
	}

	f.loaded = true;

	return true;
}

/**
 * Points MonospaceSlot at the host's monospace face, where we know one.
 *
 * Only Android is listed, because Android is the only platform this tree is
 * actually built and run on, and these paths were checked against a device
 * rather than recalled. /system/fonts is a fixed location and DroidSansMono is
 * what the "monospace" family resolves to in /system/etc/fonts.xml; RobotoMono
 * follows it rather than leading because it is not universally present -- an
 * Android 15 handset with 208 system fonts carried DroidSansMono and CutiveMono
 * and no Roboto Mono at all.
 *
 * Nothing is listed for the desktops. Font locations there vary by distribution,
 * release and packaging, and a table nobody here compiles is a guess wearing the
 * costume of a fact: it would sit in the tree looking verified and fail quietly
 * on the first machine that disagreed with it.
 *
 * Failing costs less than such a table would. The counter then draws with font
 * 0, the game's default.ttf, which is the one font the engine refuses to start
 * without. In Umineko that is Sazanami Gothic, and a Japanese face is not fixed
 * pitch taken as a whole -- its CJK glyphs are double width and its isFixedPitch
 * flag is duly 0 -- but its halfwidth Latin is. Measured over all 44 characters
 * the counter can draw, every advance is 603/1000 em against DroidSansMono's
 * 600, so the columns hold still either way and the panel width still fits.
 *
 * A title whose default.ttf breaks that loses only the steady columns.
 */
bool FontsController::loadMonospaceFont() {
#if defined(DROID)
	static const char *const candidates[]{
	    "/system/fonts/DroidSansMono.ttf",
	    "/system/fonts/RobotoMono-Regular.ttf",
	    "/system/fonts/CutiveMono.ttf",
	};

	Font &f = fonts[MonospaceSlot];

	for (const char *candidate : candidates) {
		if (!FileIO::accessFile(candidate, FileType::File))
			continue;

		// FT_New_Face rather than the stream wrapper loadFont uses: this is a
		// plain host path, not something the archive readers have to resolve.
		if (FT_New_Face(freetype, candidate, 0, &f.normal_face)) {
			f.normal_face = nullptr;
			continue;
		}

		const size_t len = std::strlen(candidate) + 1;
		f.path           = std::make_unique<char[]>(len);
		copystr(f.path.get(), candidate, len);
		f.face           = f.normal_face;
		f.loaded         = true;
		monospaceLoaded  = true;
		sendToLog(LogLevel::Info, "Monospace font for the performance counter: %s\n", candidate);
		return true;
	}

#endif

	sendToLog(LogLevel::Info, "No system monospace font, the performance counter falls back to the game default font\n");
	return false;
}

int FontsController::ownInit() {
	auto it = ons.ons_cfg_options.find("font-overrides");
	if (it != ons.ons_cfg_options.end())
		initFontOverrides(it->second);
	it = ons.ons_cfg_options.find("font-multiplier");
	if (it != ons.ons_cfg_options.end())
		initFontMultiplier(it->second);

	glyphStorageOptimisation = baseSizeMultipliers.empty() && presetSizeMultipliers.empty();

	//sendToLog(LogLevel::Info, "Initialising font system.\n");
	if (FT_Init_FreeType(&freetype))
		return -1;

	fonts_number = 0;

	for (size_t i = 0; i < 10; i++, fonts_number++) {
		if (!loadFont(fonts[i], i, false)) {
			if (i == 0)
				return -1; // default font must be loaded
			break;
		}

		//sendToLog(LogLevel::Info, "Path %i: %s\n", i, fonts[i].path);
	}

	for (size_t i = 0; i < 10; i++, user_fonts_number++) {
		if (!loadFont(user_fonts[i], i, true)) {
			break;
		}

		//sendToLog(LogLevel::Info, "Path %i: %s\n", i, fonts[i].path);
	}

	loadMonospaceFont();

	// Now need to take a basedir from default.ttf and use it as a font dir
	copystr(fontdir, fonts[0].path.get(), sizeof(fontdir));
	if (char *separator = std::strrchr(fontdir, DELIMITER))
		separator[1] = '\0';
	else
		fontdir[0] = '\0';

	return 0;
}

void FontsController::initFontOverrides(const std::string &o) {
	std::stringstream overrides(o);

	while (static_cast<void>(overrides.peek()), !overrides.eof()) {
		bool base{false};
		unsigned int src_id{0};
		unsigned int dst_id{0};
		unsigned int preset_id{0};

		if (overrides.get() == 'b') {
			base = true;
		}
		char next = overrides.peek();

		if (!base && next >= '0' && next <= '9') {
			overrides >> preset_id;
			if (overrides.get() == ':') {
				next = overrides.peek();
			} else {
				// error, invalid sequence
				return;
			}
		}

		// get src_id
		if (next >= '0' && next <= '9') {
			overrides >> src_id;
			next = overrides.get();
			if (src_id > 9)
				return; // error, invalid id
		} else {
			// error, invalid sequence
			return;
		}
		if (next == ':') {
			next = overrides.peek();
		} else {
			// error, invalid sequence
			return;
		}
		// get dst_id
		if (next >= '0' && next <= '9') {
			overrides >> dst_id;
			overrides.get();
			if (dst_id > 9)
				return; // error, invalid id
		} else {
			// error, invalid sequence
			return;
		}

		if (base) {
			baseFontOverrides[src_id] = dst_id;
		} else {
			presetFontOverrides[preset_id][src_id] = dst_id;
		}
	}
}
void FontsController::initFontMultiplier(const std::string &m) {
	std::stringstream multipliers(m);

	while (static_cast<void>(multipliers.peek()), !multipliers.eof()) {
		bool base{false};
		unsigned int src_id{0};
		float mult{0};
		unsigned int preset_id{0};

		if (multipliers.get() == 'b') {
			base = true;
		}
		char next = multipliers.peek();

		if (!base && next >= '0' && next <= '9') {
			multipliers >> preset_id;
			if (multipliers.get() == ':') {
				next = multipliers.peek();
			} else {
				// error, invalid sequence
				return;
			}
		}

		// get src_id
		if (next >= '0' && next <= '9') {
			multipliers >> src_id;
			next = multipliers.get();
			if (src_id > 9)
				return; // error, invalid id
		} else {
			// error, invalid sequence
			return;
		}
		if (next == ':') {
			next = multipliers.peek();
		} else {
			// error, invalid sequence
			return;
		}
		// get mult
		if (next >= '0' && next <= '9') {
			multipliers >> mult;
			multipliers.get();
			if (mult <= 0 || mult > 10)
				return; // error, invalid multiplier
		} else {
			// error, invalid sequence
			return;
		}

		if (base) {
			baseSizeMultipliers[src_id] = mult;
		} else {
			presetSizeMultipliers[preset_id][src_id] = mult;
		}
	}
}

int FontsController::ownDeinit() {
	if (fonts_number > 0)
		FT_Done_FreeType(freetype);
	return 0;
}

Font &FontsController::getFont(unsigned int id, int preset_id) {
	if (preset_id >= 0) {
		auto m = presetFontOverrides.find(preset_id);
		if (m != presetFontOverrides.end()) {
			auto f = m->second.find(id);
			if (f != m->second.end() && f->second <= user_fonts_number) {
				return user_fonts[f->second];
			}
		}

	} else {
		auto f = baseFontOverrides.find(id);
		if (f != baseFontOverrides.end() && f->second <= user_fonts_number) {
			return user_fonts[f->second];
		}
	}
	return fonts[id];
}

float FontsController::getMultiplier(unsigned int id, int preset_id) {
	if (preset_id >= 0) {
		auto m = presetSizeMultipliers.find(preset_id);
		if (m != presetSizeMultipliers.end()) {
			auto f = m->second.find(id);
			if (f != m->second.end()) {
				return f->second;
			}
		}
	} else {
		auto f = baseSizeMultipliers.find(id);
		if (f != baseSizeMultipliers.end()) {
			return f->second;
		}
	}

	return 1.0;
}
