@{
from rosidl_parser.definition import ACTION_FEEDBACK_SUFFIX
from rosidl_parser.definition import ACTION_GOAL_SUFFIX
from rosidl_parser.definition import ACTION_RESULT_SUFFIX
from rosidl_parser.definition import Array
from rosidl_parser.definition import AbstractGenericString
from rosidl_parser.definition import BasicType
from rosidl_parser.definition import BoundedSequence
from rosidl_parser.definition import EMPTY_STRUCTURE_REQUIRED_MEMBER_NAME
from rosidl_parser.definition import NamespacedType
from rosidl_parser.definition import AbstractSequence
from rosidl_parser.definition import UnboundedSequence

message_namespace = '::'.join(message.structure.namespaced_type.namespaces)
message_typename = '::'.join(message.structure.namespaced_type.namespaced_name())
message_fully_qualified_name = '/'.join(message.structure.namespaced_type.namespaced_name())
}@

@{
TEMPLATE(
    'msg__traits.hpp.em',
    package_name=package_name, interface_path=interface_path, message=message,
    include_directives=include_directives)
}@

namespace rosidl_generator_traits
{

template<>
inline uint32_t get_prio<@(message_typename)>(const @(message_typename) & msg)
{
  return msg.internal_rclcpp_prio;
}

template<>
inline void set_prio<@(message_typename)>(@(message_typename) & msg, uint32_t prio)
{
  msg.internal_rclcpp_prio = prio;
}

}  // namespace rosidl_generator_traits