// SPDX-License-Identifier: BSD-3-Clause
#include "geom/cad_geometry_features.hpp"

#include <algorithm>
#include <cmath>
#include <vector>

#ifdef POLYMESH_WITH_OCC

#include <BRepAdaptor_Surface.hxx>
#include <BRepBndLib.hxx>
#include <BRepGProp.hxx>
#include <Bnd_Box.hxx>
#include <GProp_GProps.hxx>
#include <TopAbs_ShapeEnum.hxx>
#include <TopExp_Explorer.hxx>
#include <TopoDS.hxx>
#include <TopoDS_Face.hxx>
#include <TopoDS_Shape.hxx>
#include <Standard_Failure.hxx>

namespace polymesh::geom {

namespace {

/// One analytic curvature radius and the face area that carries it.
struct RadiusSample {
    double radius = 0.0;
    double area = 0.0;
};

} // namespace

GeometryDescriptors compute_geometry_descriptors(const CadModel& model) {
    GeometryDescriptors out;
    if (model.empty() || model.shape_handle() == nullptr) {
        return out;
    }

    try {
        const auto& shape = *static_cast<const TopoDS_Shape*>(model.shape_handle());

        // Bounding box computed here rather than taken from CadModel::bbox_*:
        // the Python reference uses Bnd_Box over the BRep, and CadModel's box may
        // come from the tessellation. A different diagonal would rescale every
        // length-normalised descriptor and shift the Mahalanobis distance.
        Bnd_Box box;
        BRepBndLib::Add(shape, box);
        if (box.IsVoid()) {
            return out;
        }
        double xmin = 0.0;
        double ymin = 0.0;
        double zmin = 0.0;
        double xmax = 0.0;
        double ymax = 0.0;
        double zmax = 0.0;
        box.Get(xmin, ymin, zmin, xmax, ymax, zmax);

        // Ascending, matching Python's `sorted([...])`: extents[0] is the
        // shortest and the aspect ratios divide by it.
        std::vector<double> extents{xmax - xmin, ymax - ymin, zmax - zmin};
        std::sort(extents.begin(), extents.end());
        const double diag = std::sqrt(extents[0] * extents[0] + extents[1] * extents[1] +
                                      extents[2] * extents[2]);
        if (!(diag > 0.0)) {
            return out; // degenerate box; Python raises here
        }

        GProp_GProps volume_props;
        BRepGProp::VolumeProperties(shape, volume_props);
        const double volume = std::abs(volume_props.Mass());

        GProp_GProps surface_props;
        BRepGProp::SurfaceProperties(shape, surface_props);
        const double total_area = std::abs(surface_props.Mass());

        double plane_area = 0.0;
        double cyl_area = 0.0;
        double other_area = 0.0;
        std::vector<RadiusSample> radii;
        std::vector<double> areas;

        for (TopExp_Explorer it(shape, TopAbs_FACE); it.More(); it.Next()) {
            const TopoDS_Face face = TopoDS::Face(it.Current());
            GProp_GProps props;
            BRepGProp::SurfaceProperties(face, props);
            const double area = std::abs(props.Mass());
            areas.push_back(area);

            BRepAdaptor_Surface adaptor(face);
            switch (adaptor.GetType()) {
            case GeomAbs_Plane:
                plane_area += area;
                break;
            case GeomAbs_Cylinder:
                cyl_area += area;
                radii.push_back({std::abs(adaptor.Cylinder().Radius()), area});
                break;
            case GeomAbs_Sphere:
                other_area += area;
                radii.push_back({std::abs(adaptor.Sphere().Radius()), area});
                break;
            case GeomAbs_Torus:
                other_area += area;
                radii.push_back({std::abs(adaptor.Torus().MinorRadius()), area});
                break;
            case GeomAbs_Cone:
                // Deliberately no radius: a cone's radius varies along its axis,
                // so there is no single value. Python takes the same decision.
                other_area += area;
                break;
            default:
                other_area += area;
                break;
            }
        }

        if (areas.empty()) {
            return out;
        }

        const double area_norm = total_area > 0.0 ? total_area : 1.0;
        const double curved_area = cyl_area + other_area;

        double log_mean = 0.0;
        double log_std = 0.0;
        double min_radius = 1.0; // a fully planar part has no curvature scale
        if (!radii.empty()) {
            double weight_total = 0.0;
            for (const RadiusSample& sample : radii) {
                weight_total += sample.area;
            }
            // Python falls back to an unweighted average when the areas sum to
            // zero, rather than dividing by it.
            const bool weighted = weight_total > 0.0;

            min_radius = std::numeric_limits<double>::max();
            double sum = 0.0;
            for (const RadiusSample& sample : radii) {
                const double relative = sample.radius / diag;
                min_radius = std::min(min_radius, relative);
                const double weight = weighted ? sample.area / weight_total
                                               : 1.0 / static_cast<double>(radii.size());
                sum += weight * std::log10(std::max(relative, 1e-9));
            }
            log_mean = sum;

            double variance = 0.0;
            for (const RadiusSample& sample : radii) {
                const double relative = sample.radius / diag;
                const double deviation = std::log10(std::max(relative, 1e-9)) - log_mean;
                const double weight = weighted ? sample.area / weight_total
                                               : 1.0 / static_cast<double>(radii.size());
                variance += weight * deviation * deviation;
            }
            log_std = std::sqrt(std::max(variance, 0.0));
        }

        double area_sum = 0.0;
        double min_area = std::numeric_limits<double>::max();
        for (const double area : areas) {
            area_sum += area;
            min_area = std::min(min_area, area);
        }
        const double area_mean = area_sum / static_cast<double>(areas.size());
        double face_cv = 0.0;
        if (area_mean > 0.0) {
            // Population standard deviation: numpy's std() defaults to ddof=0.
            double accumulator = 0.0;
            for (const double area : areas) {
                const double deviation = area - area_mean;
                accumulator += deviation * deviation;
            }
            face_cv = std::sqrt(accumulator / static_cast<double>(areas.size())) / area_mean;
        }

        std::size_t edge_occurrences = 0;
        for (TopExp_Explorer it(shape, TopAbs_EDGE); it.More(); it.Next()) {
            ++edge_occurrences;
        }

        const double bbox_volume = extents[0] * extents[1] * extents[2];

        out.available = true;
        out.curved_area_frac = curved_area / area_norm;
        out.cyl_area_frac = cyl_area / area_norm;
        out.plane_area_frac = plane_area / area_norm;
        out.other_area_frac = other_area / area_norm;
        out.min_curv_radius_rel = min_radius;
        out.log_curv_radius_mean = log_mean;
        out.log_curv_radius_std = log_std;
        out.n_faces = static_cast<double>(areas.size());
        out.n_edges = static_cast<double>(edge_occurrences);
        out.face_area_cv = face_cv;
        out.aspect_max = extents[0] > 0.0 ? extents[2] / extents[0] : 0.0;
        out.aspect_mid = extents[0] > 0.0 ? extents[1] / extents[0] : 0.0;
        out.volume_frac = bbox_volume > 0.0 ? volume / bbox_volume : 0.0;
        out.area_over_v23 = volume > 0.0 ? total_area / std::pow(volume, 2.0 / 3.0) : 0.0;
        out.min_face_size_rel = std::sqrt(min_area) / diag;
        return out;
    } catch (const Standard_Failure&) {
        return {};
    } catch (const std::exception&) {
        return {};
    }
}

} // namespace polymesh::geom

#else

namespace polymesh::geom {

GeometryDescriptors compute_geometry_descriptors(const CadModel&) {
    return {};
}

} // namespace polymesh::geom

#endif
