#include "tracefile.h"

#include <ctime>

using namespace devel::statprofiler;
using namespace std;

#define FILE_MAGIC     "=statprofiler"
#define FORMAT_VERSION 1

#if PERL_SUBVERSION < 16
# ifndef GvNAMEUTF8
#   define GvNAMEUTF8(foo) 0
# endif
#endif

enum {
    TAG_SAMPLE_START            = 1,
    TAG_SAMPLE_END              = 2,
    TAG_SUB_FRAME               = 3,
    TAG_EVAL_FRAME              = 4,
    TAG_XSUB_FRAME              = 5,
    TAG_MAIN_FRAME              = 6,
    TAG_SECTION_START           = 198,
    TAG_SECTION_END             = 199,
    TAG_CUSTOM_META             = 200,
    TAG_META_PERL_VERSION       = 201,
    TAG_META_TICK_DURATION      = 202,
    TAG_META_STACK_SAMPLE_DEPTH = 203,
    TAG_META_LIBRARY_VERSION    = 204, // TODO implement
    TAG_META_GENEALOGY          = 205,
    TAG_HEADER_SEPARATOR        = 254,
    TAG_TAG_CONTINUATION        = 255 // just reserved for now
};

namespace {
    void append_hex(string &str, unsigned int value)
    {
        static const char digits[] = "0123456789abcdef";

        for (int i = 0; i < 8; ++i) {
            str += digits[value >> 28];
            value <<= 4;
        }
    }

    size_t varint_size(unsigned int value)
    {
        return value < (1 << 7)  ? 1 :
               value < (1 << 14) ? 2 :
               value < (1 << 21) ? 3 :
                                   4;
    }

    size_t string_size(int length)
    {
        return varint_size(length) + length;
    }

    size_t string_size(const char *value)
    {
        return string_size(value ? strlen(value) : 0);
    }

    size_t string_size(pTHX_ SV *value)
    {
        return string_size(SvCUR(value));
    }

    void skip_bytes(FILE *in, size_t size)
    {
        char buffer[128];

        for (int read = -1; read && size; ) {
            size_t to_read = min(sizeof(buffer), size);

            read = fread(buffer, 1, to_read, in);
            size -= read;

            if (to_read != read)
                croak("Unexpected end-of-file while skipping over data");
        }
    }

    void read_bytes(FILE *in, void *buffer, size_t size)
    {
        if (fread(buffer, 1, size, in) != size)
            croak("Unexpected end-of-file while reading bytes");
    }

    unsigned int read_varint(FILE *in)
    {
        int res = 0;
        int v;

        for (;;) {
            v = fgetc(in);
            if (v == EOF)
                croak("Unexpected end-of-file while reading a varint");
            res = (res << 7) | (v & 0x7f);
            if (!(v & 0x80))
                break;
        }

        return res;
    }

    SV *read_string(pTHX_ FILE *in)
    {
        int flags = fgetc(in);
        unsigned int size = read_varint(in);

        if (flags == EOF)
            croak("Unexpected end-of-file while reading a string");

        // don't return undef when length is 0
        if (size == 0)
            return sv_2mortal(newSVpvn("", 0));

        SV *sv = sv_2mortal(newSV(size));

        SvPOK_on(sv);
        SvCUR_set(sv, size);
        if (flags & 1)
            SvUTF8_on(sv);

        if (fread(SvPVX(sv), 1, size, in) != size)
            croak("Unexpected end-of-file while reading a string");

        return sv;
    }

    int write_bytes(FILE *out, const void *bytes, size_t size)
    {
        return fwrite(bytes, 1, size, out) != 0;
    }

    int write_byte(FILE *out, const char byte)
    {
        return fwrite(&byte, 1, 1, out) != 0;
    }

    int write_varint(FILE *out, unsigned int value)
    {
        char buffer[10], *curr = &buffer[sizeof(buffer) - 1];

        do {
            *curr-- = (value & 0x7f) | 0x80;
            value >>= 7;
        } while (value);

        buffer[sizeof(buffer) - 1] &= 0x7f;
        return write_bytes(out, curr + 1, (buffer + sizeof(buffer)) - (curr + 1));
    }

    int write_string(FILE *out, const char *value, size_t length, bool utf8)
    {
        int status = 0;
        status += write_byte(out, utf8 ? 1 : 0);
        status += write_varint(out, length);
        status += write_bytes(out, value, length);
        return status;
    }

    int write_string(FILE *out, const char *value, bool utf8)
    {
        return write_string(out, value, value ? strlen(value) : 0, utf8);
    }

    int write_string(FILE *out, const std::string &value, bool utf8)
    {
        return write_string(out, value.c_str(), value.length(), utf8);
    }

    int write_string(pTHX_ FILE *out, SV *value)
    {
        U32 utf8 = SvUTF8(value);
        STRLEN len;
        char *str = SvPV(value, len);
        return write_string(out, str, len, utf8);
    }

    SV *make_fullname(pTHX_ SV *package, SV *name) {
        SV *fullname = newSV(SvCUR(package) + 2 + SvCUR(name));

        SvPOK_on(fullname);
        sv_catsv(fullname, package);
        sv_catpvn(fullname, "::", 2);
        sv_catsv(fullname, name);

        return fullname;
    }
}


TraceFileReader::TraceFileReader(pTHX)
  : in(NULL), file_format_version(0), sections(NULL)
{
    SET_THX_MEMBER
    source_perl_version.revision = 0;
    source_perl_version.version = 0;
    source_perl_version.subversion = 0;
    custom_metadata = newHV();
    st_stash = gv_stashpv("Devel::StatProfiler::StackTrace", 0);
    sf_stash = gv_stashpv("Devel::StatProfiler::StackFrame", 0);
    msf_stash = gv_stashpv("Devel::StatProfiler::MainStackFrame", 0);
    esf_stash = gv_stashpv("Devel::StatProfiler::EvalStackFrame", 0);
}

TraceFileReader::~TraceFileReader()
{
    SvREFCNT_dec(custom_metadata);
    SvREFCNT_dec(sections);
    close();
}

void TraceFileReader::open(const std::string &path)
{
    SvREFCNT_dec(sections);
    sections = newHV();
    close();
    in = fopen(path.c_str(), "r");
    read_header();
}

void TraceFileReader::read_header()
{
    char magic[sizeof(FILE_MAGIC) - 1];

    if (fread(magic, 1, sizeof(magic), in) != sizeof(magic))
        croak("Unexpected end-of-file while reading file magic");
    if (strncmp(magic, FILE_MAGIC, sizeof(magic)))
        croak("Invalid file magic");

    // In future, will check that the version is at least not newer
    // than this library's file format version. That's necessary even
    // if there's a backcompat layer.
    unsigned int version_from_file = read_varint(in);
    if (version_from_file < 1 || version_from_file > FORMAT_VERSION)
        croak("Incompatible file format version %i", version_from_file);

    file_format_version = (unsigned int)version_from_file;

    // TODO this becomes a loop reading header records
    bool cont = 1;
    while (cont) {
        const int tag = fgetc(in);

        switch (tag) {
        case EOF:
            croak("Invalid input file: File ends before end of file header");
        case TAG_HEADER_SEPARATOR:
            cont = 0;
            break;

        case TAG_META_PERL_VERSION: {
            source_perl_version.revision   = read_varint(in);
            source_perl_version.version    = read_varint(in);
            source_perl_version.subversion = read_varint(in);
            break;
        }
        case TAG_META_TICK_DURATION: {
            source_tick_duration = read_varint(in);
            break;
        }
        case TAG_META_STACK_SAMPLE_DEPTH: {
            source_stack_sample_depth = read_varint(in);
            break;
        }
        case TAG_META_GENEALOGY: {
            genealogy_info.ordinal = read_varint(in);
            genealogy_info.parent_ordinal = read_varint(in);
            read_bytes(in, genealogy_info.id, sizeof(genealogy_info.id));
            read_bytes(in, genealogy_info.parent_id, sizeof(genealogy_info.parent_id));
            break;
        }
        case TAG_CUSTOM_META:
            read_custom_meta_record(read_varint(in));
            break;

        default:
            croak("Invalid input file: Invalid header record tag (%i)", tag);
        }
    }
}

void TraceFileReader::read_custom_meta_record(const int size, HV *extra_output_hash)
{
    SV *key = read_string(aTHX_ in);
    SV *value = read_string(aTHX_ in);
    SvREFCNT_inc(value);
    hv_store_ent(custom_metadata, key, value, 0);

    if (extra_output_hash) {
        SvREFCNT_inc(value);
        hv_store_ent(extra_output_hash, key, value, 0);
    }
}

void TraceFileReader::close()
{
    if (in)
        fclose(in);
    in = NULL;
}

SV *TraceFileReader::read_trace()
{
    HV *sample = NULL;
    AV *frames;

    // As we read more meta data, we'll build up this hash (which is
    // created lazily below). If there's any, that hash will be returned
    // with the trace as a hash element of the trace ("metadata").
    // We also insert any metadata found into the TraceFileReader global
    // metadata hash.
    HV *new_metadata = NULL;

    for (;;) {
        int type = fgetc(in);

        if (type == EOF)
            return newSV(0);

        int size = read_varint(in);
        switch (type) {
        case TAG_SAMPLE_START: {
            unsigned int weight = read_varint(in);
            SV *op_name = read_string(aTHX_ in);

            sample = (HV *) sv_2mortal((SV *) newHV());
            frames = newAV();

            hv_stores(sample, "frames", newRV_noinc((SV *) frames));
            hv_stores(sample, "weight", newSViv(weight));
            hv_stores(sample, "op_name", SvREFCNT_inc(op_name));
            break;
        }
        default:
            warn("Unknown record type in trace file. Attempting to skip this record");
            skip_bytes(in, size);
            break;
        case TAG_SUB_FRAME: {
            if (!sample)
                croak("Invalid input file: Found stray sub-frame tag without sample-start tag");
            SV *package = read_string(aTHX_ in);
            SV *name = read_string(aTHX_ in);
            SV *file = read_string(aTHX_ in);
            int line = read_varint(in);
            HV *frame = newHV();

            hv_stores(frame, "fq_sub_name", make_fullname(aTHX_ package, name));
            hv_stores(frame, "package", SvREFCNT_inc(package));
            hv_stores(frame, "sub_name", SvREFCNT_inc(name));
            hv_stores(frame, "file", SvREFCNT_inc(file));
            hv_stores(frame, "line", newSViv(line));
            av_push(frames, sv_bless(newRV_noinc((SV *) frame), sf_stash));

            break;
        }
        case TAG_XSUB_FRAME: {
            if (!sample)
                croak("Invalid input file: Found stray sub-frame tag without sample-start tag");
            SV *package = read_string(aTHX_ in);
            SV *name = read_string(aTHX_ in);
            HV *frame = newHV();

            hv_stores(frame, "fq_sub_name", make_fullname(aTHX_ package, name));
            hv_stores(frame, "package", SvREFCNT_inc(package));
            hv_stores(frame, "sub_name", SvREFCNT_inc(name));
            hv_stores(frame, "file", newSVpvn("", 0));
            hv_stores(frame, "line", newSViv(-1));
            av_push(frames, sv_bless(newRV_noinc((SV *) frame), sf_stash));

            break;
        }
        case TAG_EVAL_FRAME: {
            if (!sample)
                croak("Invalid input file: Found stray sub-frame tag without sample-start tag");
            SV *file = read_string(aTHX_ in);
            int line = read_varint(in);
            HV *frame = newHV();

            hv_stores(frame, "file", SvREFCNT_inc(file));
            hv_stores(frame, "line", newSViv(line));
            av_push(frames, sv_bless(newRV_noinc((SV *) frame), esf_stash));

            break;
        }
        case TAG_MAIN_FRAME: {
            if (!sample)
                croak("Invalid input file: Found stray sub-frame tag without sample-start tag");
            SV *file = read_string(aTHX_ in);
            int line = read_varint(in);
            HV *frame = newHV();

            hv_stores(frame, "file", SvREFCNT_inc(file));
            hv_stores(frame, "line", newSViv(line));
            av_push(frames, sv_bless(newRV_noinc((SV *) frame), msf_stash));

            break;
        }
        case TAG_SAMPLE_END:
            if (!sample)
                croak("Invalid input file: Found stray sample-end tag without sample-start tag");
            skip_bytes(in, size);

            hv_stores(sample, "active_sections", newRV_inc((SV *)sections));
            if (new_metadata)
                hv_stores(sample, "metadata", newRV_inc((SV *)new_metadata));
            return sv_bless(newRV_inc((SV *) sample), st_stash);
        case TAG_CUSTOM_META:
            if (!new_metadata)
                new_metadata = (HV *)sv_2mortal((SV *)newHV());
            read_custom_meta_record(size, new_metadata);
            break;
        case TAG_SECTION_START: {
            SV *section_name = read_string(aTHX_ in);
            HE *depth = hv_fetch_ent(sections, section_name, 1, 0);
            if (!depth || !SvOK(HeVAL(depth)))
                sv_setuv(HeVAL(depth), 1);
            else
                sv_setuv(HeVAL(depth), 1 + SvUV(HeVAL(depth)));
            break;
        }
        case TAG_SECTION_END: {
            SV *section_name = read_string(aTHX_ in);
            HE *depth= hv_fetch_ent(sections, section_name, 0, 0);
            if (!depth || !SvOK(HeVAL(depth)) || SvUV(HeVAL(depth)) == 0) {
                STRLEN len;
                char *str = SvPV(section_name, len);
                croak("Invalid input file: Unmatched section end for '%.*s'", len, str);
            }

            const UV depth_num = SvUV(HeVAL(depth));
            if (depth_num == 1)
                hv_delete_ent(sections, section_name, G_DISCARD, 0);
            else
                sv_setuv(HeVAL(depth), depth_num - 1);
            break;
        }
        } // end switch
    }
}

HV *TraceFileReader::get_custom_metadata()
{
    return custom_metadata;
}


TraceFileWriter::TraceFileWriter(pTHX) :
    out(NULL), force_empty_frame(false)
{
    SET_THX_MEMBER
}

TraceFileWriter::~TraceFileWriter()
{
    close();
}

long TraceFileWriter::position() const
{
    return ftell(out);
}

int TraceFileWriter::open(const std::string &path, bool is_template, unsigned int id[ID_SIZE], unsigned int ordinal)
{
    close();
    output_file = path;

    if (is_template) {
        output_file += '.';
        for (int i = 0; i < 6; ++i)
            append_hex(output_file, id[i]);

        output_file += '.';
        append_hex(output_file, ordinal);
    }

    out = fopen(output_file.c_str(), "w");
    if (!out)
        return 1;

    return 0;
}

int TraceFileWriter::write_perl_version()
{
    int status = 0;
    status += write_byte(out, TAG_META_PERL_VERSION);
    status += write_varint(out, PERL_REVISION);
    status += write_varint(out, PERL_VERSION);
    status += write_varint(out, PERL_SUBVERSION);
    return status;
}

int TraceFileWriter::write_header(unsigned int sampling_interval,
                                  unsigned int stack_collect_depth,
                                  unsigned int id[ID_SIZE], unsigned int ordinal,
                                  unsigned int parent_id[ID_SIZE], unsigned int parent_ordinal)
{
    int status = 0;
    status += write_bytes(out, FILE_MAGIC, sizeof(FILE_MAGIC) - 1);
    status += write_varint(out, FORMAT_VERSION);

    // Write meta data: Perl version, tick duration, stack sample depth
    status += write_perl_version();

    status += write_byte(out, TAG_META_TICK_DURATION);
    status += write_varint(out, sampling_interval);

    status += write_byte(out, TAG_META_STACK_SAMPLE_DEPTH);
    status += write_varint(out, stack_collect_depth);

    status += write_byte(out, TAG_META_GENEALOGY);
    status += write_varint(out, ordinal);
    status += write_varint(out, parent_ordinal);
    status += write_bytes(out, id, sizeof(id[0]) * ID_SIZE);
    status += write_bytes(out, parent_id, sizeof(parent_id[0]) * ID_SIZE);

    status += write_byte(out, TAG_HEADER_SEPARATOR);
    return status;
}

void TraceFileWriter::close()
{
    flush();

    if (out) {
        string temp = output_file + "_";

        fclose(out);
        if (!rename(temp.c_str(), output_file.c_str()))
            unlink(temp.c_str());
    }

    out = NULL;
}

void TraceFileWriter::shut()
{
    fclose(out);

    force_empty_frame = false;
    out = NULL;
}

void TraceFileWriter::flush()
{
    if (force_empty_frame) {
        start_sample(0, NULL);
        end_sample();
    }

    fflush(out);
}

int TraceFileWriter::start_sample(unsigned int weight, OP *current_op)
{
    // TODO maybe track whether we're already in a sample so we can barf if we
    // generate nested samples in error? This would also allow for forbidding
    // adding section info within a sample, which makes no sense.
    const char *op_name = current_op ? OP_NAME(current_op) : NULL;
    int status = 0;

    status += write_byte(out, TAG_SAMPLE_START);
    status += write_varint(out, varint_size(weight) + string_size(op_name));
    status += write_varint(out, weight);
    status += write_string(out, op_name, false);

    return status;
}

int TraceFileWriter::start_section(SV *section_name)
{
    // TODO possibly track sections fully to forbid generating invalid sections
    int status = 0;

    status += write_byte(out, TAG_SECTION_START);
    status += write_varint(out, string_size(aTHX_ section_name));
    status += write_string(aTHX_ out, section_name);
    force_empty_frame = false;

    return status;
}

int TraceFileWriter::end_section(SV *section_name)
{
    // TODO possibly track sections fully to forbid generating invalid sections
    int status = 0;

    status += write_byte(out, TAG_SECTION_END);
    status += write_varint(out, string_size(aTHX_ section_name));
    status += write_string(aTHX_ out, section_name);
    force_empty_frame = true;

    return status;
}

int TraceFileWriter::add_frame(FrameType frame_type, CV *sub, GV *sub_name, COP *line)
{
    const char *file;
    size_t file_size;
    int lineno, status = 0;

    if (frame_type != FRAME_XSUB) {
        file = OutCopFILE(line);
        file_size = strlen(file);
        lineno = CopLINE(line);
    }

    // require: cx->blk_eval.old_namesv
    // mPUSHs(newSVsv(cx->blk_eval.old_namesv));

    if (frame_type == FRAME_SUB || frame_type == FRAME_XSUB) {
        const char *package = "__ANON__", *name = "(unknown)";
        bool package_utf8 = false, name_utf8 = false;
        size_t package_size = 8, name_size = 9;

        // from Perl_pp_caller
	GV * const cvgv = sub_name ? sub_name : CvGV(sub);
	if (cvgv && isGV(cvgv)) {
            GV *egv = GvEGVx(cvgv) ? GvEGVx(cvgv) : cvgv;
            HV *stash = GvSTASH(egv);

            if (stash) {
                package = HvNAME(stash);
#if PERL_SUBVERSION >= 16
                package_utf8 = HvNAMEUTF8(stash);
                package_size = HvNAMELEN(stash);
#else
                package_utf8 = 0;
                package_size = strlen(package);
#endif
            }
            name = GvNAME(egv);
            name_utf8 = GvNAMEUTF8(egv);
            name_size = GvNAMELEN(egv);
	}

        if (frame_type == FRAME_SUB) {
            status += write_byte(out, TAG_SUB_FRAME);
            status += write_varint(out, string_size(package_size) +
                                        string_size(name_size) +
                                        string_size(file_size) +
                                        varint_size(lineno));
            status += write_string(out, package, package_size, package_utf8);
            status += write_string(out, name, name_size, name_utf8);
            status += write_string(out, file, file_size, false);
            status += write_varint(out, lineno);
        } else {
            status += write_byte(out, TAG_XSUB_FRAME);
            status += write_varint(out, string_size(package_size) +
                                        string_size(name_size));
            status += write_string(out, package, package_size, package_utf8);
            status += write_string(out, name, name_size, name_utf8);
        }
    } else if (frame_type == FRAME_EVAL) {
        status += write_byte(out, TAG_EVAL_FRAME);
        status += write_varint(out, string_size(file_size) +
                                    varint_size(lineno));
        status += write_string(out, file, file_size, false);
        status += write_varint(out, lineno);
    } else {
        status += write_byte(out, TAG_MAIN_FRAME);
        status += write_varint(out, string_size(file_size) +
                                    varint_size(lineno));
        status += write_string(out, file, file_size, false);
        status += write_varint(out, lineno);
    }

    return status;
}

int TraceFileWriter::end_sample()
{
    int status = 0;
    status += write_byte(out, TAG_SAMPLE_END);
    status += write_varint(out, 0);
    force_empty_frame = false;
    return status;
}

int TraceFileWriter::write_custom_metadata(SV *key, SV *value)
{
    int status = 0;
    status += write_byte(out, TAG_CUSTOM_META);
    status += write_varint(out, SvCUR(key) + SvCUR(value));
    status += write_string(aTHX_ out, key);
    status += write_string(aTHX_ out, value);
    force_empty_frame = true;
    return status;
}

