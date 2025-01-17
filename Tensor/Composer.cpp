//
//  Composer.cpp
//  Tensor
//
//  Created by 陳均豪 on 2022/10/7.
//

#include "Composer.hpp"

#include "NanodetPlusDetectionOutputLayer.hpp"
#include "TensorTransform.hpp"
#include "PoseEstimation.hpp"

namespace otter {
namespace cv {

Composer::Composer(const char* nanodet_param, const char* nanodet_weight, const char* simplepose_param, const char* simplepose_weight, bool object_stable, bool pose_stable) {
    init(nanodet_param, nanodet_weight, simplepose_param, simplepose_param, object_stable, pose_stable);
}

void Composer::init(const char* nanodet_param, const char* nanodet_weight, const char* simplepose_param, const char* simplepose_weight, bool object_stable, bool pose_stable) {
    nanodet.load_otter(nanodet_param, otter::CompileMode::Inference);
    nanodet.load_weight(nanodet_weight, otter::Net::WeightType::Ncnn);
    
    simplepose.load_otter(simplepose_param, otter::CompileMode::Inference);
    simplepose.load_weight(simplepose_weight, otter::Net::WeightType::Ncnn);
    
    enable_pose_stabilizer = pose_stable;
    enable_object_stabilizer = object_stable;
    target_size = 416;
}

void Composer::set_pose_stabilizer(bool option) {
    enable_pose_stabilizer = option;
}

void Composer::set_object_stabilizer(bool option) {
    enable_object_stabilizer = option;
}

void Composer::set_detection_size(int size) {
    target_size = size;
}

void Composer::detect(Tensor frame) {
    int frame_width = frame.size(3);
    int frame_height = frame.size(2);
    
    float scale;
    int wpad, hpad;
    auto nanodet_pre_process = otter::nanodet_pre_process(frame, target_size, scale, wpad, hpad);
    
    auto nanodet_extractor = nanodet.create_extractor();
        
    nanodet_extractor.input("data_1", nanodet_pre_process);
    otter::Tensor nanodet_predict;
    nanodet_extractor.extract("nanodet", nanodet_predict, 0);
    auto nanodet_post_process = otter::nanodet_post_process(nanodet_predict, frame_width, frame_height, scale, wpad, hpad);
    
    // Normalize width and height
    nanodet_post_process.slice(1, 2, 5, 2) /= frame_width;
    nanodet_post_process.slice(1, 3, 6, 2) /= frame_height;
    
    // Stabilize
    stabilized_objects = nanodet_post_process;
    
    if (enable_object_stabilizer) {
        std::vector<otter::Object> objects = from_tensor_to_object(stabilized_objects);
        
        auto tracking_box = object_stabilizer.track(objects);
        
        std::vector<otter::Object> tracking_objects;
        
        for (auto& box : tracking_box) {
            tracking_objects.push_back(box.obj);
        }
        
        stabilized_objects = from_object_to_tensor(tracking_objects);
    }
    
    // Finding the target
    int target_index = observer.getTarget(stabilized_objects);
    
    if (target_index != -1) {
        auto object_data = stabilized_objects.accessor<float, 2>();
        
        if (object_data[target_index][0] == 1) {   // If detect the person
            auto target_object = stabilized_objects[target_index].clone();
            target_object.slice(0, 2, 5, 2) *= frame_width;
            target_object.slice(0, 3, 6, 2) *= frame_height;
            
            auto simplepose_input = pose_pre_process(target_object, frame);
                        
            auto simplepose_extractor = simplepose.create_extractor();
            simplepose_extractor.input("data_1", simplepose_input.image);
                        
            otter::Tensor simplepose_predict;
            simplepose_extractor.extract("conv_56", simplepose_predict, 0);
                        
            keypoints = otter::pose_post_process(simplepose_predict, simplepose_input);
            
            // Normalize
            for (auto& keypoint : keypoints) {
                keypoint.p.x /= frame_width;
                keypoint.p.y /= frame_height;
            }
            
            // Stabilize
            if (enable_pose_stabilizer) {
                keypoints = pose_stabilizer.track(keypoints);
            }
        } else {
            keypoints.clear();
        }
    } else {
        pose_stabilizer.reset();
        keypoints.clear();
    }
}

void Composer::predict() {
    // predict keypoint
    if (enable_pose_stabilizer) {
        keypoints = pose_stabilizer.predict();
    }
}

std::vector<Object> from_tensor_to_object(Tensor& objs) {
    std::vector<Object> objects;
    
    for (int i = 0; i < objs.size(0); ++i) {
        objects.push_back(Object(objs[i]));
    }
    
    return objects;
}

Tensor from_object_to_tensor(std::vector<Object> objs) {
    auto objects = otter::empty({static_cast<long long>(objs.size()), 6}, otter::ScalarType::Float);
    auto object_a = objects.accessor<float, 2>();
    
    for (int i = 0; i < objs.size(); ++i) {
        auto object = object_a[i];
        object[0] = objs[i].label;
        object[1] = objs[i].prob;
        object[2] = objs[i].rect.x;
        object[3] = objs[i].rect.y;
        object[4] = objs[i].rect.width;
        object[5] = objs[i].rect.height;
    }
    
    return objects;
}

}   // end namespace cv
}   // end namespace otter
