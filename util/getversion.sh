#!/bin/sh
#
# This file is part of the flashrom project.
#
# Copyright (C) 2009,2010 Carl-Daniel Hailfinger
# Copyright (C) 2011 Chromium OS Authors
#
# This program is free software; you can redistribute it and/or modify
# it under the terms of the GNU General Public License as published by
# the Free Software Foundation; either version 2 of the License, or
# (at your option) any later version.
#
# This program is distributed in the hope that it will be useful,
# but WITHOUT ANY WARRANTY; without even the implied warranty of
# MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
# GNU General Public License for more details.
#
# You should have received a copy of the GNU General Public License
# along with this program; if not, write to the Free Software
# Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA  02110-1301 USA
#
# getversion.sh: Get version / revision info. Currently, only Git and Subversion
# are supported.
#
# *_revision:   Echo the latest source code revision (git hash or svn revision).
#
# *_server:     Echo the server from where the source code was checked out.
#
# *_timestamp:  Echo the timestamp of most recent modification. If the sources
#               are pristine, the timestamp will correspond to the most recently
#               committed change, expressed in UTC. If there are local
#               modifications, the timestamp will correspond to compilation
#               time and a '+' is added to denote uncommitted changes.
#

git_revision() {
  echo $(git log --pretty=format:'%h' -n 1)
}

git_server() {
  # Note: This may not work as expected if multiple remotes are fetched from.
  echo $(git remote -v | \
         awk '/fetch/ {split($2, pieces, "//"); print pieces[2]; exit 0}')
}

git_timestamp() {
  local date_format="+%b %d %Y %H:%M:%S"
  local timestamp

  # are there local changes in the client?
  if git status | \
    egrep '^# Change(s to be committed|d but not updated):$' > /dev/null
  then
    timestamp=$(date "${date_format} +")
  else
    # No local changes, get date of the last log record.
    timestamp=$(git log --pretty=format:'%ct' -n 1 | \
                awk '{print strftime("%b %d %Y %H:%M:%S UTC", $1, 1)}')
  fi

  echo "${timestamp}"
}

svn_revision() {
  local revision=""

  # Try both "svnversion" and "svn info"
  revision=$(LC_ALL=C svnversion -cn . 2>/dev/null | \
             sed -e "s/.*://" -e "s/\([0-9]*\).*/\1/" | grep "[0-9]")

  if [ -z "${revision}" ]; then
    revision=$(LC_ALL=C svn info . 2>/dev/null | \
               awk '/^Revision:/ {print $2 }' | grep "[0-9]")
  fi

  # Prepend revision with 'r'
  echo "r${revision}"
}

svn_server() {
  echo $(LC_ALL=C svn info . 2>/dev/null | awk '/^URL:/ {print $2 }')
}

svn_timestamp() {
  local date_format="+%b %d %Y %H:%M:%S"
  local timestamp

  # Whitelist most of the characters that appear on the left of
  # "svn status" output, with the notable exceptions being '?' (Not
  # under version control) and 'X'(item is an externals definition)
  if [ -n "$(LC_ALL=C svn status . | grep '^ *[ADMRCI\!L+SKOTB\*].*')" ]; then
    timestamp=$(date "${date_format} +")
  else
    local last_commit_date=$(svn info . | \
      grep "Last Changed Date:" | awk '{print $4" "$5" "$6}')
    timestamp=$(date --utc --date "${last_commit_date}" "${date_format} UTC")
  fi

  echo "${timestamp}"
}

#
# Determine which SCM we're using and choose the appropriate functions
#
IS_GIT=0
IS_SVN=0

if [ -d ".git" ]; then
  IS_GIT=1
elif [ -d ".svn" ]; then
  IS_SVN=1
fi

get_revision() {
  if [ $IS_GIT -eq 1 ]; then
    echo "$(git_revision)"
  elif [ $IS_SVN -eq 1 ]; then
    echo "$(svn_revision)"
  fi
}

get_server() {
  if [ $IS_GIT -eq 1 ]; then
    echo "$(git_server)"
  elif [ $IS_SVN -eq 1 ]; then
    echo "$(svn_server)"
  fi
}

get_timestamp() {
  if [ $IS_GIT -eq 1 ]; then
    echo "$(git_timestamp)"
  elif [ $IS_SVN -eq 1 ]; then
    echo "$(svn_timestamp)"
  fi
}

show_help() {
  echo "Usage: getversion.sh <options>"
  echo ""
  echo "Options:"
  echo -e "\t-r | --revision     Revision (SVN), commit hash (Git)"
  echo -e "\t-s | --server       Upstream server"
  echo -e "\t-t | --timestamp    UTC timestamp of most recent modification"
}

if [ -z "$*" ]; then
  # default action: print "<revision> : <server> : <timestamp>"
  echo "${0}: Using default parameters" >&2
  echo "$(get_server) : $(get_revision) : $(get_timestamp)"
else
  for ARG in $@; do
    case ${ARG} in
      -h|--help)
        show_help;
        shift;;
      -r|--revision)
        echo "$(get_revision)"
        shift;;
      -s|--server)
        echo "$(get_server)"
        shift;;
      -t|--timestamp)
        echo "$(get_timestamp)"
        shift;;
    esac;
  done
fi
