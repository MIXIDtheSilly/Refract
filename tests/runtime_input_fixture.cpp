#include "pose_client.h"
refract::protocol::PoseFrame& refract_input_fixture() {
    static refract::protocol::PoseFrame frame;
    return frame;
}
namespace refract::runtime {
refract::protocol::PoseFrame PoseClient::latest_pose_frame() { return refract_input_fixture(); }
PoseClient& pose_client() { static PoseClient client; return client; }
}
