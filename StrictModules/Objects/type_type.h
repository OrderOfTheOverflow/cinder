#ifndef __STRICTM_TYPE_TYPE_H__
#define __STRICTM_TYPE_TYPE_H__

#include "StrictModules/Objects/object_type.h"

namespace strictmod::objects {
class StrictTypeType : public StrictObjectType {
 public:
  using StrictObjectType::StrictObjectType;

  virtual std::shared_ptr<BaseStrictObject> loadAttr(
      std::shared_ptr<BaseStrictObject> obj,
      const std::string& key,
      std::shared_ptr<BaseStrictObject> defaultValue,
      const CallerContext& caller) override;

  virtual void storeAttr(
      std::shared_ptr<BaseStrictObject> obj,
      const std::string& key,
      std::shared_ptr<BaseStrictObject> value,
      const CallerContext& caller) override;
};
} // namespace strictmod::objects

#endif //__STRICTM_TYPE_TYPE_H__
