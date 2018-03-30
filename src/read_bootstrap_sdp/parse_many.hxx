#pragma once

#include "parse_append_many.hxx"

template <class T>
std::vector<T>
parse_many(const std::string &name,
           T (*parse_function)(const boost::property_tree::ptree &),
           const boost::property_tree::ptree &tree)
{
  std::vector<T> result;
  parse_append_many(name, parse_function, tree, result);
  return result;
}