// SPDX-License-Identifier: BSD-3-Clause
// Read-only BRep evidence: inspect_brep and exact trimmed-face sampling.
#include "geom/cad_model.hpp"

#include <cstddef>

#ifdef POLYMESH_WITH_OCC

#include <BRepAdaptor_Surface.hxx>
#include <BRepCheck_Analyzer.hxx>
#include <BRepClass_FaceClassifier.hxx>
#include <BRepGProp.hxx>
#include <BRepTools.hxx>
#include <BRep_Tool.hxx>
#include <GProp_GProps.hxx>
#include <Precision.hxx>
#include <TopAbs.hxx>
#include <TopExp.hxx>
#include <TopExp_Explorer.hxx>
#include <TopTools_IndexedMapOfShape.hxx>
#include <TopoDS.hxx>
#include <TopoDS_Face.hxx>
#include <TopoDS_Shape.hxx>
#include <TopoDS_Shell.hxx>
#include <TopoDS_Vertex.hxx>
#include <gp_Pnt.hxx>
#include <gp_Pnt2d.hxx>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <format>
#include <vector>

#endif // POLYMESH_WITH_OCC

namespace polymesh::geom {

#ifdef POLYMESH_WITH_OCC

BRepInspection inspect_brep(const CadModel& model) {
    BRepInspection out;
    if (model.empty() || model.shape_handle() == nullptr) {
        return out;
    }

    const auto& shape = *static_cast<const TopoDS_Shape*>(model.shape_handle());
    out.available = true;
    out.valid = BRepCheck_Analyzer(shape).IsValid();

    TopTools_IndexedMapOfShape solids;
    TopTools_IndexedMapOfShape shells;
    TopTools_IndexedMapOfShape faces;
    TopTools_IndexedMapOfShape edges;
    TopTools_IndexedMapOfShape vertices;
    TopExp::MapShapes(shape, TopAbs_SOLID, solids);
    TopExp::MapShapes(shape, TopAbs_SHELL, shells);
    TopExp::MapShapes(shape, TopAbs_FACE, faces);
    TopExp::MapShapes(shape, TopAbs_EDGE, edges);
    TopExp::MapShapes(shape, TopAbs_VERTEX, vertices);
    out.solid_count = static_cast<std::size_t>(solids.Extent());
    out.shell_count = static_cast<std::size_t>(shells.Extent());
    out.face_count = static_cast<std::size_t>(faces.Extent());
    out.edge_count = static_cast<std::size_t>(edges.Extent());
    out.vertex_count = static_cast<std::size_t>(vertices.Extent());

    for (Standard_Integer i = 1; i <= shells.Extent(); ++i) {
        if (BRep_Tool::IsClosed(TopoDS::Shell(shells(i)))) {
            ++out.closed_shell_count;
        }
    }
    out.closed = out.shell_count > 0 && out.closed_shell_count == out.shell_count;

    GProp_GProps volume_props;
    BRepGProp::VolumeProperties(shape, volume_props);
    out.volume = std::abs(static_cast<double>(volume_props.Mass()));

    GProp_GProps surface_props;
    BRepGProp::SurfaceProperties(shape, surface_props);
    out.surface_area = std::abs(static_cast<double>(surface_props.Mass()));
    return out;
}

BRepSurfaceSamples sample_brep_surface(const CadModel& model, std::size_t max_samples) {
    BRepSurfaceSamples result;
    if (model.empty() || model.shape_handle() == nullptr) {
        return result;
    }
    const auto& shape = *static_cast<const TopoDS_Shape*>(model.shape_handle());
    TopTools_IndexedMapOfShape mapped_faces;
    TopExp::MapShapes(shape, TopAbs_FACE, mapped_faces);
    const std::size_t face_count = static_cast<std::size_t>(mapped_faces.Extent());
    result.face_count = face_count;
    if (face_count == 0) {
        return result;
    }
    if (max_samples < face_count) {
        throw GeomError(
            std::format("sample_brep_surface: max_samples={} cannot cover {} BRep faces",
                        max_samples, face_count));
    }

    // Give every face one point, then distribute the remaining budget by exact
    // surface area. Stable fractional-remainder ordering makes the allocation
    // deterministic while approximating an area-weighted surface distribution.
    std::vector<std::size_t> quotas(face_count, 1);
    std::vector<double> areas(face_count, 0.0);
    double total_area = 0.0;
    for (std::size_t i = 0; i < face_count; ++i) {
        GProp_GProps properties;
        BRepGProp::SurfaceProperties(TopoDS::Face(mapped_faces(static_cast<int>(i + 1))),
                                     properties);
        const double area = std::abs(static_cast<double>(properties.Mass()));
        if (std::isfinite(area) && area > 0.0) {
            areas[i] = area;
            total_area += area;
        }
    }
    const std::size_t remaining = max_samples - face_count;
    if (remaining > 0 && total_area > 0.0 && std::isfinite(total_area)) {
        std::vector<double> fractions(face_count, 0.0);
        std::size_t allocated = 0;
        for (std::size_t i = 0; i < face_count; ++i) {
            const long double exact =
                static_cast<long double>(remaining) *
                (static_cast<long double>(areas[i]) / static_cast<long double>(total_area));
            const std::size_t available = remaining - allocated;
            const long double floored = std::floor(exact);
            const std::size_t whole =
                !std::isfinite(exact) || floored >= static_cast<long double>(available)
                    ? available
                    : static_cast<std::size_t>(std::max(0.0L, floored));
            quotas[i] += whole;
            allocated += whole;
            fractions[i] = std::isfinite(exact) ? static_cast<double>(exact - floored) : 0.0;
        }
        std::vector<std::size_t> order(face_count);
        for (std::size_t i = 0; i < face_count; ++i) {
            order[i] = i;
        }
        std::stable_sort(order.begin(), order.end(),
                         [&fractions](std::size_t a, std::size_t b) {
                             return fractions[a] > fractions[b];
                         });
        for (std::size_t i = 0; i < remaining - allocated; ++i) {
            ++quotas[order[i % face_count]];
        }
    } else if (remaining > 0) {
        for (std::size_t i = 0; i < face_count; ++i) {
            quotas[i] += remaining / face_count;
            if (i < remaining % face_count) {
                ++quotas[i];
            }
        }
    }

    std::vector<Eigen::Vector3d>& samples = result.points;
    result.face_ids.reserve(max_samples);
    samples.reserve(max_samples);
    for (std::size_t i = 0; i < face_count; ++i) {
        const TopoDS_Face face =
            TopoDS::Face(mapped_faces(static_cast<Standard_Integer>(i + 1)));
        const std::size_t quota = quotas[i];
        const std::size_t before = samples.size();

        Standard_Real u_min = 0.0;
        Standard_Real u_max = 0.0;
        Standard_Real v_min = 0.0;
        Standard_Real v_max = 0.0;
        BRepTools::UVBounds(face, u_min, u_max, v_min, v_max);
        const bool finite_bounds = std::isfinite(static_cast<double>(u_min)) &&
                                   std::isfinite(static_cast<double>(u_max)) &&
                                   std::isfinite(static_cast<double>(v_min)) &&
                                   std::isfinite(static_cast<double>(v_max)) &&
                                   u_max > u_min && v_max > v_min;
        if (finite_bounds) {
            // A ceil(2*sqrt(quota)) grid attempts at most 9*quota points.
            // Cell centres avoid over-counting coincident face boundaries.
            const std::size_t grid_side = static_cast<std::size_t>(
                std::ceil(2.0 * std::sqrt(static_cast<double>(quota))));
            BRepAdaptor_Surface surface(face, Standard_True);
            for (std::size_t v = 0; v < grid_side && samples.size() - before < quota; ++v) {
                const double fv =
                    (static_cast<double>(v) + 0.5) / static_cast<double>(grid_side);
                const Standard_Real param_v =
                    v_min + static_cast<Standard_Real>(fv) * (v_max - v_min);
                for (std::size_t u = 0; u < grid_side && samples.size() - before < quota;
                     ++u) {
                    const double fu =
                        (static_cast<double>(u) + 0.5) / static_cast<double>(grid_side);
                    const Standard_Real param_u =
                        u_min + static_cast<Standard_Real>(fu) * (u_max - u_min);
                    BRepClass_FaceClassifier classifier(face, gp_Pnt2d(param_u, param_v),
                                                        Precision::Confusion());
                    ++result.uv_attempt_count;
                    const TopAbs_State state = classifier.State();
                    if (state != TopAbs_IN && state != TopAbs_ON) {
                        continue;
                    }
                    const gp_Pnt point = surface.Value(param_u, param_v);
                    if (std::isfinite(static_cast<double>(point.X())) &&
                        std::isfinite(static_cast<double>(point.Y())) &&
                        std::isfinite(static_cast<double>(point.Z()))) {
                        samples.emplace_back(point.X(), point.Y(), point.Z());
                        result.face_ids.push_back(static_cast<std::uint32_t>(i));
                    }
                }
            }
        }

        if (samples.size() == before) {
            // Thin/degenerate trims can miss every bounded cell centre. A BRep
            // vertex remains an exact point on the trimmed face.
            TopExp_Explorer vertices(face, TopAbs_VERTEX);
            bool found_finite_vertex = false;
            while (vertices.More()) {
                const gp_Pnt point = BRep_Tool::Pnt(TopoDS::Vertex(vertices.Current()));
                if (std::isfinite(static_cast<double>(point.X())) &&
                    std::isfinite(static_cast<double>(point.Y())) &&
                    std::isfinite(static_cast<double>(point.Z()))) {
                    samples.emplace_back(point.X(), point.Y(), point.Z());
                    result.face_ids.push_back(static_cast<std::uint32_t>(i));
                    found_finite_vertex = true;
                    ++result.fallback_vertex_count;
                    break;
                }
                vertices.Next();
            }
            if (!found_finite_vertex) {
                throw GeomError(std::format(
                    "sample_brep_surface: face {} yielded no finite bounded exact sample", i));
            }
        }
    }
    return result;
}

#else // !POLYMESH_WITH_OCC

BRepInspection inspect_brep(const CadModel& /*model*/) { return {}; }

BRepSurfaceSamples sample_brep_surface(const CadModel& /*model*/,
                                       std::size_t /*max_samples*/) {
    return {};
}

#endif // POLYMESH_WITH_OCC

} // namespace polymesh::geom
