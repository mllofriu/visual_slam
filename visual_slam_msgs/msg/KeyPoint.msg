# Taken from rtabmap_ros/KeyPoint.msg

#class cv::KeyPoint
#{
#    Point2f pt;
#    float size;
#    float angle;
#    float response;
#    int octave;
#    int class_id;
#}

float32 x
float32 y
float32 size
float32 angle
float32 response
int32 octave
int32 class_id

uint8[32] descriptor
