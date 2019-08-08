/*
 * Copyright 2016 The Cartographer Authors
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include "cartographer/mapping/map_builder.h"
#include <fstream>
#include "cartographer/common/make_unique.h"
#include "cartographer/common/time.h"
#include "cartographer/io/internal/mapping_state_serialization.h"
#include "cartographer/io/proto_stream_deserializer.h"
#include "cartographer/mapping/internal/2d/local_trajectory_builder_2d.h"
#include "cartographer/mapping/internal/2d/overlapping_submaps_trimmer_2d.h"
#include "cartographer/mapping/internal/2d/pose_graph_2d.h"
#include "cartographer/mapping/internal/3d/local_trajectory_builder_3d.h"
#include "cartographer/mapping/internal/3d/pose_graph_3d.h"
#include "cartographer/mapping/internal/collated_trajectory_builder.h"
#include "cartographer/mapping/internal/global_trajectory_builder.h"
#include "cartographer/mapping/proto/internal/legacy_serialized_data.pb.h"
#include "cartographer/sensor/internal/collator.h"
#include "cartographer/sensor/internal/trajectory_collator.h"
#include "cartographer/sensor/internal/voxel_filter.h"
#include "cartographer/transform/rigid_transform.h"
#include "cartographer/transform/transform.h"

namespace cartographer {
namespace mapping {

    namespace {

        using mapping::proto::SerializedData;

        std::vector<std::string> SelectRangeSensorIds(
                const std::set<MapBuilder::SensorId> &expected_sensor_ids) {
            std::vector<std::string> range_sensor_ids;
            for (const MapBuilder::SensorId &sensor_id : expected_sensor_ids) {
                if (sensor_id.type == MapBuilder::SensorId::SensorType::RANGE) {
                    range_sensor_ids.push_back(sensor_id.id);
                }
            }
            return range_sensor_ids;
        }

    }  // namespace

    proto::MapBuilderOptions CreateMapBuilderOptions(
            common::LuaParameterDictionary *const parameter_dictionary) {
        proto::MapBuilderOptions options;
        options.set_use_trajectory_builder_2d(
                parameter_dictionary->GetBool("use_trajectory_builder_2d"));
        options.set_use_trajectory_builder_3d(
                parameter_dictionary->GetBool("use_trajectory_builder_3d"));
        options.set_num_background_threads(
                parameter_dictionary->GetNonNegativeInt("num_background_threads"));
        *options.mutable_pose_graph_options() = CreatePoseGraphOptions(
                parameter_dictionary->GetDictionary("pose_graph").get());
        CHECK_NE(options.use_trajectory_builder_2d(),
                 options.use_trajectory_builder_3d());
        return options;
    }

    MapBuilder::MapBuilder(const proto::MapBuilderOptions &options)
            : options_(options), thread_pool_(options.num_background_threads()) {
        CHECK(options.use_trajectory_builder_2d() ^
              options.use_trajectory_builder_3d());
        if (options.use_trajectory_builder_2d()) {
            pose_graph_ = common::make_unique<PoseGraph2D>(
                    options_.pose_graph_options(),
                    common::make_unique<optimization::OptimizationProblem2D>(
                            options_.pose_graph_options().optimization_problem_options()),
                    &thread_pool_);
        }
        if (options.use_trajectory_builder_3d()) {
            pose_graph_ = common::make_unique<PoseGraph3D>(
                    options_.pose_graph_options(),
                    common::make_unique<optimization::OptimizationProblem3D>(
                            options_.pose_graph_options().optimization_problem_options()),
                    &thread_pool_);
        }
        if (options.collate_by_trajectory()) {
            sensor_collator_ = common::make_unique<sensor::TrajectoryCollator>();
        } else {
            sensor_collator_ = common::make_unique<sensor::Collator>();
        }
    }

    int MapBuilder::AddTrajectoryBuilder(
            const std::set<SensorId> &expected_sensor_ids,
            const proto::TrajectoryBuilderOptions &trajectory_options,
            LocalSlamResultCallback local_slam_result_callback) {
        const int trajectory_id = trajectory_builders_.size();
        if (options_.use_trajectory_builder_3d()) {
            std::unique_ptr<LocalTrajectoryBuilder3D> local_trajectory_builder;
            if (trajectory_options.has_trajectory_builder_3d_options()) {
                local_trajectory_builder = common::make_unique<LocalTrajectoryBuilder3D>(
                        trajectory_options.trajectory_builder_3d_options(),
                        SelectRangeSensorIds(expected_sensor_ids));
            }
            DCHECK(dynamic_cast<PoseGraph3D *>(pose_graph_.get()));
            trajectory_builders_.push_back(
                    common::make_unique<CollatedTrajectoryBuilder>(
                            sensor_collator_.get(), trajectory_id, expected_sensor_ids,
                            CreateGlobalTrajectoryBuilder3D(
                                    std::move(local_trajectory_builder), trajectory_id,
                                    static_cast<PoseGraph3D *>(pose_graph_.get()),
                                    local_slam_result_callback)));
        } else {
            std::unique_ptr<LocalTrajectoryBuilder2D> local_trajectory_builder;
            if (trajectory_options.has_trajectory_builder_2d_options()) {
                local_trajectory_builder = common::make_unique<LocalTrajectoryBuilder2D>(
                        trajectory_options.trajectory_builder_2d_options(),
                        SelectRangeSensorIds(expected_sensor_ids));
            }
            DCHECK(dynamic_cast<PoseGraph2D *>(pose_graph_.get()));
            trajectory_builders_.push_back(
                    common::make_unique<CollatedTrajectoryBuilder>(
                            sensor_collator_.get(), trajectory_id, expected_sensor_ids,
                            CreateGlobalTrajectoryBuilder2D(
                                    std::move(local_trajectory_builder), trajectory_id,
                                    static_cast<PoseGraph2D *>(pose_graph_.get()),
                                    local_slam_result_callback)));

            if (trajectory_options.has_overlapping_submaps_trimmer_2d()) {
                const auto &trimmer_options =
                        trajectory_options.overlapping_submaps_trimmer_2d();
                pose_graph_->AddTrimmer(common::make_unique<OverlappingSubmapsTrimmer2D>(
                        trimmer_options.fresh_submaps_count(),
                        trimmer_options.min_covered_area() /
                        common::Pow2(trajectory_options.trajectory_builder_2d_options()
                                             .submaps_options()
                                             .grid_options_2d()
                                             .resolution()),
                        trimmer_options.min_added_submaps_count()));
            }
        }
        if (trajectory_options.pure_localization()) {
            constexpr int kSubmapsToKeep = 3;
            pose_graph_->AddTrimmer(common::make_unique<PureLocalizationTrimmer>(
                    trajectory_id, kSubmapsToKeep));
        }
        if (trajectory_options.has_initial_trajectory_pose()) {
            const auto &initial_trajectory_pose =
                    trajectory_options.initial_trajectory_pose();
            pose_graph_->SetInitialTrajectoryPose(
                    trajectory_id, initial_trajectory_pose.to_trajectory_id(),
                    transform::ToRigid3(initial_trajectory_pose.relative_pose()),
                    common::FromUniversal(initial_trajectory_pose.timestamp()));
        }
        proto::TrajectoryBuilderOptionsWithSensorIds options_with_sensor_ids_proto;
        for (const auto &sensor_id : expected_sensor_ids) {
            *options_with_sensor_ids_proto.add_sensor_id() = ToProto(sensor_id);
        }
        *options_with_sensor_ids_proto.mutable_trajectory_builder_options() =
                trajectory_options;
        all_trajectory_builder_options_.push_back(options_with_sensor_ids_proto);
        CHECK_EQ(trajectory_builders_.size(), all_trajectory_builder_options_.size());
        return trajectory_id;
    }

    int MapBuilder::AddTrajectoryForDeserialization(
            const proto::TrajectoryBuilderOptionsWithSensorIds &
            options_with_sensor_ids_proto) {
        const int trajectory_id = trajectory_builders_.size();
        trajectory_builders_.emplace_back();
        all_trajectory_builder_options_.push_back(options_with_sensor_ids_proto);
        CHECK_EQ(trajectory_builders_.size(), all_trajectory_builder_options_.size());
        return trajectory_id;
    }

    void MapBuilder::FinishTrajectory(const int trajectory_id) {
        sensor_collator_->FinishTrajectory(trajectory_id);
        pose_graph_->FinishTrajectory(trajectory_id);
    }

    std::string MapBuilder::LocalSubmapToProto(proto::SubmapQuery::Response *const response) {
        auto submap_data = pose_graph_->GetLocalCurrentSubmap();
        if (submap_data.submap == nullptr) {
            return "Requested LocalSubmap "
                   " but it does not exist: maybe it has not been setted.";
        }
        transform::Rigid3d pose(Eigen::Vector3d(0,0,0),Eigen::Quaterniond::Identity());
        submap_data.submap->ToResponseProto(pose, response);
        return "";
    }

    std::string MapBuilder::SubmapToProto(
            const SubmapId &submap_id, proto::SubmapQuery::Response *const response) {
        if (submap_id.trajectory_id < 0 ||
            submap_id.trajectory_id >= num_trajectory_builders()) {
            return "Requested submap from trajectory " +
                   std::to_string(submap_id.trajectory_id) + " but there are only " +
                   std::to_string(num_trajectory_builders()) + " trajectories.";
        }

        const auto submap_data = pose_graph_->GetSubmapData(submap_id);
        if (submap_data.submap == nullptr) {
            return "Requested submap " + std::to_string(submap_id.submap_index) +
                   " from trajectory " + std::to_string(submap_id.trajectory_id) +
                   " but it does not exist: maybe it has been trimmed.";
        }
        submap_data.submap->ToResponseProto(submap_data.pose, response);
        //std::cout<<"global submap pose: "<<submap_data.pose<<std::endl;
        return "";
    }

    void MapBuilder::SerializeState(io::ProtoStreamWriterInterface *const writer) {
        io::WritePbStream(*pose_graph_, all_trajectory_builder_options_, writer);
    }

    void MapBuilder::LoadState(io::ProtoStreamReaderInterface *const reader,
                               bool load_frozen_state) {
        io::ProtoStreamDeserializer deserializer(reader);
        // Create a copy of the pose_graph_proto, such that we can re-write the
        // trajectory ids.
        proto::PoseGraph pose_graph_proto = deserializer.pose_graph();
        const auto &all_builder_options_proto =
                deserializer.all_trajectory_builder_options();

        std::map<int, int> trajectory_remapping;
        for (auto &trajectory_proto : *pose_graph_proto.mutable_trajectory()) {
            const auto &options_with_sensor_ids_proto =
                    all_builder_options_proto.options_with_sensor_ids(
                            trajectory_proto.trajectory_id());
            const int new_trajectory_id =
                    AddTrajectoryForDeserialization(options_with_sensor_ids_proto);
            CHECK(trajectory_remapping
                          .emplace(trajectory_proto.trajectory_id(), new_trajectory_id)
                          .second)
            << "Duplicate trajectory ID: " << trajectory_proto.trajectory_id();
            trajectory_proto.set_trajectory_id(new_trajectory_id);
            if (load_frozen_state) {
                pose_graph_->FreezeTrajectory(new_trajectory_id);
            }
        }

        // Apply the calculated remapping to constraints in the pose graph proto.
        for (auto &constraint_proto : *pose_graph_proto.mutable_constraint()) {
            constraint_proto.mutable_submap_id()->set_trajectory_id(
                    trajectory_remapping.at(constraint_proto.submap_id().trajectory_id()));
            constraint_proto.mutable_node_id()->set_trajectory_id(
                    trajectory_remapping.at(constraint_proto.node_id().trajectory_id()));
        }
        MapById<SubmapId, transform::Rigid3d> submap_poses;

        for (const proto::Trajectory &trajectory_proto :
                pose_graph_proto.trajectory()) {
            for (const proto::Trajectory::Submap &submap_proto :
                    trajectory_proto.submap()) {
                submap_poses.Insert(SubmapId{trajectory_proto.trajectory_id(),
                                             submap_proto.submap_index()},
                                    transform::ToRigid3(submap_proto.pose()));
            }
        }

        MapById<NodeId, transform::Rigid3d> node_poses;
        for (const proto::Trajectory &trajectory_proto :
                pose_graph_proto.trajectory()) {
            for (const proto::Trajectory::Node &node_proto : trajectory_proto.node()) {
                node_poses.Insert(
                        NodeId{trajectory_proto.trajectory_id(), node_proto.node_index()},
                        transform::ToRigid3(node_proto.pose()));
            }
        }

        // Set global poses of landmarks.
        for (const auto &landmark : pose_graph_proto.landmark_poses()) {
            pose_graph_->SetLandmarkPose(landmark.landmark_id(),
                                         transform::ToRigid3(landmark.global_pose()));
        }

        SerializedData proto;
        while (deserializer.ReadNextSerializedData(&proto)) {
            switch (proto.data_case()) {
                case SerializedData::kPoseGraph:
                    LOG(ERROR) << "Found multiple serialized `PoseGraph`. Serialized "
                                  "stream likely corrupt!.";
                    break;
                case SerializedData::kAllTrajectoryBuilderOptions:
                    LOG(ERROR) << "Found multiple serialized "
                                  "`AllTrajectoryBuilderOptions`. Serialized stream likely "
                                  "corrupt!.";
                    break;
                case SerializedData::kSubmap: {
                    proto.mutable_submap()->mutable_submap_id()->set_trajectory_id(
                            trajectory_remapping.at(
                                    proto.submap().submap_id().trajectory_id()));
                    const transform::Rigid3d &submap_pose = submap_poses.at(
                            SubmapId{proto.submap().submap_id().trajectory_id(),
                                     proto.submap().submap_id().submap_index()});
                    pose_graph_->AddSubmapFromProto(submap_pose, proto.submap());
                    break;
                }
                case SerializedData::kNode: {
                    proto.mutable_node()->mutable_node_id()->set_trajectory_id(
                            trajectory_remapping.at(proto.node().node_id().trajectory_id()));
                    const transform::Rigid3d &node_pose =
                            node_poses.at(NodeId{proto.node().node_id().trajectory_id(),
                                                 proto.node().node_id().node_index()});
                    pose_graph_->AddNodeFromProto(node_pose, proto.node());
                    break;
                }
                case SerializedData::kTrajectoryData: {
                    proto.mutable_trajectory_data()->set_trajectory_id(
                            trajectory_remapping.at(proto.trajectory_data().trajectory_id()));
                    pose_graph_->SetTrajectoryDataFromProto(proto.trajectory_data());
                    break;
                }
                case SerializedData::kImuData: {
                    if (load_frozen_state) break;
                    pose_graph_->AddImuData(
                            trajectory_remapping.at(proto.imu_data().trajectory_id()),
                            sensor::FromProto(proto.imu_data().imu_data()));
                    break;
                }
                case SerializedData::kOdometryData: {
                    if (load_frozen_state) break;
                    pose_graph_->AddOdometryData(
                            trajectory_remapping.at(proto.odometry_data().trajectory_id()),
                            sensor::FromProto(proto.odometry_data().odometry_data()));
                    break;
                }
                case SerializedData::kFixedFramePoseData: {
                    if (load_frozen_state) break;
                    pose_graph_->AddFixedFramePoseData(
                            trajectory_remapping.at(
                                    proto.fixed_frame_pose_data().trajectory_id()),
                            sensor::FromProto(
                                    proto.fixed_frame_pose_data().fixed_frame_pose_data()));
                    break;
                }
                case SerializedData::kLandmarkData: {
                    if (load_frozen_state) break;
                    pose_graph_->AddLandmarkData(
                            trajectory_remapping.at(proto.landmark_data().trajectory_id()),
                            sensor::FromProto(proto.landmark_data().landmark_data()));
                    break;
                }
                default:
                    LOG(WARNING) << "Skipping unknown message type in stream: "
                                 << proto.GetTypeName();
            }
        }

        if (load_frozen_state) {
            // Add information about which nodes belong to which submap.
            // Required for 3D pure localization.
            for (const proto::PoseGraph::Constraint &constraint_proto :
                    pose_graph_proto.constraint()) {
                if (constraint_proto.tag() !=
                    proto::PoseGraph::Constraint::INTRA_SUBMAP) {
                    continue;
                }
                pose_graph_->AddNodeToSubmap(
                        NodeId{constraint_proto.node_id().trajectory_id(),
                               constraint_proto.node_id().node_index()},
                        SubmapId{constraint_proto.submap_id().trajectory_id(),
                                 constraint_proto.submap_id().submap_index()});
            }
        } else {
            // When loading unfrozen trajectories, 'AddSerializedConstraints' will
            // take care of adding information about which nodes belong to which
            // submap.
            pose_graph_->AddSerializedConstraints(
                    FromProto(pose_graph_proto.constraint()));
        }
        CHECK(reader->eof());
    }

    void MapBuilder::LoadStateLandmark(io::ProtoStreamReaderInterface *const reader,
                               const std::string landmark_poses_file,
                               bool load_frozen_state) {
        io::ProtoStreamDeserializer deserializer(reader);
        // Create a copy of the pose_graph_proto, such that we can re-write the
        // trajectory ids.
        proto::PoseGraph pose_graph_proto = deserializer.pose_graph();
        const auto &all_builder_options_proto =
                deserializer.all_trajectory_builder_options();

        std::map<int, int> trajectory_remapping;
        for (auto &trajectory_proto : *pose_graph_proto.mutable_trajectory()) {
            const auto &options_with_sensor_ids_proto =
                    all_builder_options_proto.options_with_sensor_ids(
                            trajectory_proto.trajectory_id());
            const int new_trajectory_id =
                    AddTrajectoryForDeserialization(options_with_sensor_ids_proto);
            CHECK(trajectory_remapping
                          .emplace(trajectory_proto.trajectory_id(), new_trajectory_id)
                          .second)
            << "Duplicate trajectory ID: " << trajectory_proto.trajectory_id();
            trajectory_proto.set_trajectory_id(new_trajectory_id);
            if (load_frozen_state) {
                pose_graph_->FreezeTrajectory(new_trajectory_id);
            }
        }

        // Apply the calculated remapping to constraints in the pose graph proto.
        for (auto &constraint_proto : *pose_graph_proto.mutable_constraint()) {
            constraint_proto.mutable_submap_id()->set_trajectory_id(
                    trajectory_remapping.at(constraint_proto.submap_id().trajectory_id()));
            constraint_proto.mutable_node_id()->set_trajectory_id(
                    trajectory_remapping.at(constraint_proto.node_id().trajectory_id()));
        }
        MapById<SubmapId, transform::Rigid3d> submap_poses;

        for (const proto::Trajectory &trajectory_proto :
                pose_graph_proto.trajectory()) {
            for (const proto::Trajectory::Submap &submap_proto :
                    trajectory_proto.submap()) {
                submap_poses.Insert(SubmapId{trajectory_proto.trajectory_id(),
                                             submap_proto.submap_index()},
                                    transform::ToRigid3(submap_proto.pose()));
            }
        }

        MapById<NodeId, transform::Rigid3d> node_poses;
        for (const proto::Trajectory &trajectory_proto :
                pose_graph_proto.trajectory()) {
            for (const proto::Trajectory::Node &node_proto : trajectory_proto.node()) {
                node_poses.Insert(
                        NodeId{trajectory_proto.trajectory_id(), node_proto.node_index()},
                        transform::ToRigid3(node_proto.pose()));
            }
        }

        // Set global poses of landmarks.
        for (const auto &landmark : pose_graph_proto.landmark_poses()) {
            pose_graph_->SetLandmarkPose(landmark.landmark_id(),
                                         transform::ToRigid3(landmark.global_pose()));
        }
        // add by galyean, to add camera poses:
        {
            std::ifstream read_landmark(landmark_poses_file);
            char buffer[1024];
            while(read_landmark.getline(buffer,1024)){
                int index;
                double value[7];
                std::stringstream ss;
                ss<<buffer;
                ss>>index;
                for(size_t i=0;i!=7;++i) {
                    ss >> value[i];
                }
                Eigen::Vector3d translation(value[0],value[1],value[2]);
                Eigen::Quaterniond rotation(value[6],value[3],value[4],value[5]);
                transform::Rigid3d trans(translation,rotation);
                pose_graph_->SetLandmarkPose(std::to_string(index),trans);
            }
        }
        SerializedData proto;
        while (deserializer.ReadNextSerializedData(&proto)) {
            switch (proto.data_case()) {
                case SerializedData::kPoseGraph:
                    LOG(ERROR) << "Found multiple serialized `PoseGraph`. Serialized "
                                  "stream likely corrupt!.";
                    break;
                case SerializedData::kAllTrajectoryBuilderOptions:
                    LOG(ERROR) << "Found multiple serialized "
                                  "`AllTrajectoryBuilderOptions`. Serialized stream likely "
                                  "corrupt!.";
                    break;
                case SerializedData::kSubmap: {
                    proto.mutable_submap()->mutable_submap_id()->set_trajectory_id(
                            trajectory_remapping.at(
                                    proto.submap().submap_id().trajectory_id()));
                    const transform::Rigid3d &submap_pose = submap_poses.at(
                            SubmapId{proto.submap().submap_id().trajectory_id(),
                                     proto.submap().submap_id().submap_index()});
                    pose_graph_->AddSubmapFromProto(submap_pose, proto.submap());
                    break;
                }
                case SerializedData::kNode: {
                    proto.mutable_node()->mutable_node_id()->set_trajectory_id(
                            trajectory_remapping.at(proto.node().node_id().trajectory_id()));
                    const transform::Rigid3d &node_pose =
                            node_poses.at(NodeId{proto.node().node_id().trajectory_id(),
                                                 proto.node().node_id().node_index()});
                    pose_graph_->AddNodeFromProto(node_pose, proto.node());
                    break;
                }
                case SerializedData::kTrajectoryData: {
                    proto.mutable_trajectory_data()->set_trajectory_id(
                            trajectory_remapping.at(proto.trajectory_data().trajectory_id()));
                    pose_graph_->SetTrajectoryDataFromProto(proto.trajectory_data());
                    break;
                }
                case SerializedData::kImuData: {
                    if (load_frozen_state) break;
                    pose_graph_->AddImuData(
                            trajectory_remapping.at(proto.imu_data().trajectory_id()),
                            sensor::FromProto(proto.imu_data().imu_data()));
                    break;
                }
                case SerializedData::kOdometryData: {
                    if (load_frozen_state) break;
                    pose_graph_->AddOdometryData(
                            trajectory_remapping.at(proto.odometry_data().trajectory_id()),
                            sensor::FromProto(proto.odometry_data().odometry_data()));
                    break;
                }
                case SerializedData::kFixedFramePoseData: {
                    if (load_frozen_state) break;
                    pose_graph_->AddFixedFramePoseData(
                            trajectory_remapping.at(
                                    proto.fixed_frame_pose_data().trajectory_id()),
                            sensor::FromProto(
                                    proto.fixed_frame_pose_data().fixed_frame_pose_data()));
                    break;
                }
                case SerializedData::kLandmarkData: {
                    if (load_frozen_state) break;
                    pose_graph_->AddLandmarkData(
                            trajectory_remapping.at(proto.landmark_data().trajectory_id()),
                            sensor::FromProto(proto.landmark_data().landmark_data()));
                    break;
                }
                default:
                    LOG(WARNING) << "Skipping unknown message type in stream: "
                                 << proto.GetTypeName();
            }
        }

        if (load_frozen_state) {
            // Add information about which nodes belong to which submap.
            // Required for 3D pure localization.
            for (const proto::PoseGraph::Constraint &constraint_proto :
            pose_graph_proto.constraint()) {
                if (constraint_proto.tag() !=
                    proto::PoseGraph::Constraint::INTRA_SUBMAP) {
                    continue;
                }
                pose_graph_->AddNodeToSubmap(
                        NodeId{constraint_proto.node_id().trajectory_id(),
                               constraint_proto.node_id().node_index()},
                        SubmapId{constraint_proto.submap_id().trajectory_id(),
                                 constraint_proto.submap_id().submap_index()});
            }
        } else {
            // When loading unfrozen trajectories, 'AddSerializedConstraints' will
            // take care of adding information about which nodes belong to which
            // submap.
            pose_graph_->AddSerializedConstraints(
                    FromProto(pose_graph_proto.constraint()));
        }
        CHECK(reader->eof());
    }
}  // namespace mapping
}  // namespace cartographer