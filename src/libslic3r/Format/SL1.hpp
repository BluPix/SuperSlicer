#ifndef ARCHIVETRAITS_HPP
#define ARCHIVETRAITS_HPP

#include <string>

#include "libslic3r/Zipper.hpp"
#include "libslic3r/SLAPrint.hpp"
#include "libslic3r/Format/SLAArchive.hpp"

namespace Slic3r {

class SL1Archive: public SLAArchive {
    SLAPrinterConfig m_cfg;
    
protected:
    SLAPrinterConfig& config() override { return m_cfg; }
    const SLAPrinterConfig& config() const override { return m_cfg; }

public:
    
    SL1Archive() = default;
    explicit SL1Archive(const SLAPrinterConfig &cfg): m_cfg(cfg) {}
    explicit SL1Archive(SLAPrinterConfig &&cfg): m_cfg(std::move(cfg)) {}
    
    void export_print(Zipper &zipper, const SLAPrint &print, const std::string &projectname = "") override;
};
    
void import_sla_archive(const std::string &zipfname, DynamicPrintConfig &out);

void import_sla_archive(
    const std::string &      zipfname,
    Vec2i32                  windowsize,
    TriangleMesh &           out,
    DynamicPrintConfig &     profile,
    std::function<bool(int)> progr = [](int) { return true; });

inline void import_sla_archive(
    const std::string &      zipfname,
    Vec2i32                  windowsize,
    TriangleMesh &           out,
    std::function<bool(int)> progr = [](int) { return true; })
{
    DynamicPrintConfig profile;
    import_sla_archive(zipfname, windowsize, out, profile, progr);
}

} // namespace Slic3r::sla

#endif // ARCHIVETRAITS_HPP
