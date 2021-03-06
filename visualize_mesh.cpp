#pragma once

#ifndef MKL_BLAS
#define MKL_BLAS MKL_DOMAIN_BLAS
#endif

#define EIGEN_USE_MKL_ALL

#include <eigen3/Eigen/Dense>
#include <eigen3/Eigen/Geometry>
#include <eigen3/Eigen/LU>

#include <QDir>

#include "ceres/ceres.h"

#include <opencv2/opencv.hpp>

#include "basicmesh.h"
#include "common.h"
#include "constraints.h"
#include "costfunctions.h"
#include "ioutilities.h"
#include "multilinearmodel.h"
#include "parameters.h"
#include "singleimagereconstructor.hpp"
#include "statsutils.h"
#include "utils.hpp"

#include "OffscreenMeshVisualizer.h"

#include "AAM/aammodel.h"

#include "boost/filesystem/operations.hpp"
#include "boost/filesystem/path.hpp"
#include "boost/program_options.hpp"

namespace fs = boost::filesystem;
namespace po = boost::program_options;

using namespace Eigen;

po::variables_map parse_cli_args(int argc, char** argv) {
  const string home_directory = QDir::homePath().toStdString();
  cout << "Home dir: " << home_directory << endl;

  po::options_description desc("Options");
  desc.add_options()
    ("help", "Print help messages")
    ("img", po::value<string>()->required(), "Background iamge.")
    ("res", po::value<string>()->required(), "Reconstruction information.")
    ("mesh", po::value<string>()->default_value(""), "Mesh to render.")
    ("output_mesh", po::value<string>()->default_value(""), "Saved mesh filename.")
    ("init_bs_path", po::value<string>()->default_value(""), "Initial blendshapes path.")
    ("faces", po::value<string>(), "Faces to render")
    ("ambient_occlusion", po::value<string>(), "AO for the mesh.")
    ("texture", po::value<string>(), "Texture for the mesh.")
    ("normals", po::value<string>(), "Customized normals for the mesh.")
    ("no_subdivision", "Perform subdivision for mesh")
    ("init", "Is the initial multi-recon")
    ("settings", po::value<string>()->default_value(home_directory + "/Data/Settings/mesh_vis.json"), "Rendering settings")
    ("output", po::value<string>()->required(), "Output image file.");
  po::variables_map vm;

  try {
    po::store(po::parse_command_line(argc, argv, desc), vm);
    po::notify(vm);
    return vm;
  } catch(po::error& e) {
    cerr << "Error: " << e.what() << endl;
    cerr << desc << endl;
    exit(1);
  }
}

void VisualizeReconstructionResult(
  const string& img_filename,
  const string& res_filename,
  const string& mesh_filename,
  const string& init_bs_path,
  const string& output_image_filename,
  bool no_subdivision,
  const map<string, string>& extra_options,
  bool scale_output=true) {

  QImage img(img_filename.c_str());
  int imgw = img.width();
  int imgh = img.height();
  if(scale_output) {
    const int target_size = 640;
    double scale = static_cast<double>(target_size) / imgw;
    imgw *= scale;
    imgh *= scale;
  }

  auto recon_results = LoadReconstructionResult(res_filename);

  BasicMesh mesh;
  if(!mesh_filename.empty()) {
    cout << "Using mesh directly ..." << endl;
    mesh.LoadOBJMesh(mesh_filename);
    mesh.ComputeNormals();
  } else {
    const int num_blendshapes = 46;
    vector<BasicMesh> blendshapes(num_blendshapes+1);
  #pragma omp parallel for
    for(int i=0;i<=num_blendshapes;++i) {
      if(extra_options.count("init"))
        blendshapes[i].LoadOBJMesh( init_bs_path + "/" + "Binit_" + to_string(i) + ".obj" );
      else
        blendshapes[i].LoadOBJMesh( init_bs_path + "/" + "B_" + to_string(i) + ".obj" );
      blendshapes[i].ComputeNormals();
    }

    mesh = blendshapes[0];

    MatrixX3d verts0 = blendshapes[0].vertices();
    MatrixX3d verts = verts0;
    for(int j=1;j<=num_blendshapes;++j) {
      verts += (blendshapes[j].vertices() - verts0) * recon_results.params_model.Wexp_FACS(j);
    }
    mesh.vertices() = verts;
    mesh.ComputeNormals();
  }

  if(extra_options.count("output_mesh")) mesh.Write(extra_options.at("output_mesh"));

  OffscreenMeshVisualizer visualizer(imgw, imgh);

  visualizer.SetMVPMode(OffscreenMeshVisualizer::CamPerspective);
  visualizer.SetRenderMode(OffscreenMeshVisualizer::MeshAndImage);
  visualizer.BindMesh(mesh);
  visualizer.BindImage(img);

  visualizer.SetCameraParameters(recon_results.params_cam);
  visualizer.SetMeshRotationTranslation(recon_results.params_model.R, recon_results.params_model.T);
  visualizer.SetIndexEncoded(false);
  visualizer.SetEnableLighting(true);

  if(extra_options.count("settings")) visualizer.LoadRenderingSettings(extra_options.at("settings"));
  if(extra_options.count("texture")) visualizer.BindTexture(QImage(extra_options.at("texture").c_str()));
  if(extra_options.count("normals")) {
    visualizer.SetNormals(LoadFloats(extra_options.at("normals")));
  }
  if(extra_options.count("ambient_occlusion")) {
    visualizer.SetAmbientOcclusion(LoadFloats(extra_options.at("ambient_occlusion")));
  }
  if(extra_options.count("faces")) {
    auto hair_region_indices_quad = LoadIndices(extra_options.at("faces"));
    vector<int> hair_region_indices;
    // @HACK each quad face is triangulated, so the indices change from i to [2*i, 2*i+1]
    for(auto fidx : hair_region_indices_quad) {
      hair_region_indices.push_back(fidx*2);
      hair_region_indices.push_back(fidx*2+1);
    }
    // HACK: each valid face i becomes [4i, 4i+1, 4i+2, 4i+3] after the each
    // subdivision. See BasicMesh::Subdivide for details
    const int max_subdivisions = no_subdivision?0:1;
    for(int i=0;i<max_subdivisions;++i) {
      vector<int> hair_region_indices_new;
      for(auto fidx : hair_region_indices) {
        int fidx_base = fidx*4;
        hair_region_indices_new.push_back(fidx_base);
        hair_region_indices_new.push_back(fidx_base+1);
        hair_region_indices_new.push_back(fidx_base+2);
        hair_region_indices_new.push_back(fidx_base+3);
      }
      hair_region_indices = hair_region_indices_new;
    }
    visualizer.SetFacesToRender(hair_region_indices);
  }

  QImage output_img = visualizer.Render(true);
  cout << "Writing output image to " << output_image_filename << endl;
  cout << "Image size: " << output_img.width() << 'x' << output_img.height() << endl;
  output_img.save(output_image_filename.c_str());
}

int main(int argc, char** argv) {
  QApplication app(argc, argv);
  auto vm = parse_cli_args(argc, argv);
  if(argc<5) {
    cout << "Usage: " << argv[0] << " img res mesh output" << endl;
    return 1;
  }

  map<string, string> extra_options;
  if(vm.count("normals")) extra_options.insert({"normals", vm["normals"].as<string>()});
  if(vm.count("texture")) extra_options.insert({"texture", vm["texture"].as<string>()});
  if(vm.count("settings")) extra_options.insert({"settings", vm["settings"].as<string>()});
  if(vm.count("faces")) extra_options.insert({"faces", vm["faces"].as<string>()});
  if(vm.count("ambient_occlusion")) extra_options.insert({"ambient_occlusion", vm["ambient_occlusion"].as<string>()});
  if(vm.count("init")) extra_options.insert({"init", "true"});
  if(vm.count("output_mesh")) extra_options.insert({"output_mesh", vm["output_mesh"].as<string>()});

  VisualizeReconstructionResult(vm["img"].as<string>(),
                                vm["res"].as<string>(),
                                vm["mesh"].as<string>(),
                                vm["init_bs_path"].as<string>(),
                                vm["output"].as<string>(),
                                vm.count("no_subdivision"),
                                extra_options);
  return 0;
}
